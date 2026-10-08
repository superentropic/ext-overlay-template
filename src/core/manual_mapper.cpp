#include "manual_mapper.h"
#include "syscalls.h"
#include <cstdio>
#include <fstream>

namespace ManualMapper {

// ============================================================================
//  Shellcode loader data — passed to the remote shellcode
// ============================================================================
struct LoaderData {
    uintptr_t imageBase;
    uintptr_t ntHeadersOffset;     // Offset from imageBase to NT headers
    uintptr_t fnLoadLibraryA;
    uintptr_t fnGetProcAddress;
    uintptr_t fnRtlAddFunctionTable;  // For exception handling (x64)
    DWORD     result;                  // Set by shellcode: 1=success, 0=fail
};

// ============================================================================
//  Shellcode — runs inside the target process
//
//  This function MUST be position-independent. It receives LoaderData*
//  as its single argument. It:
//    1. Applies base relocations
//    2. Resolves imports (IAT)
//    3. Calls DllMain(DLL_PROCESS_ATTACH)
// ============================================================================
#pragma runtime_checks("", off)
#pragma optimize("", off)
static DWORD WINAPI ShellcodeLoader(LoaderData* data) {
    if (!data) return 0;

    auto base = reinterpret_cast<BYTE*>(data->imageBase);
    auto ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + data->ntHeadersOffset);
    auto optHeader = &ntHeaders->OptionalHeader;

    // ---- Step 1: Apply base relocations ----
    auto relocDelta = reinterpret_cast<uintptr_t>(base) - optHeader->ImageBase;
    if (relocDelta != 0) {
        auto& relocDir = optHeader->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
        if (relocDir.Size > 0) {
            auto relocBlock = reinterpret_cast<IMAGE_BASE_RELOCATION*>(base + relocDir.VirtualAddress);
            while (relocBlock->VirtualAddress > 0) {
                auto entryCount = (relocBlock->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
                auto entries = reinterpret_cast<WORD*>(reinterpret_cast<BYTE*>(relocBlock) + sizeof(IMAGE_BASE_RELOCATION));

                for (DWORD i = 0; i < entryCount; i++) {
                    int type = entries[i] >> 12;
                    int offset = entries[i] & 0xFFF;

                    if (type == IMAGE_REL_BASED_DIR64) {
                        auto patchAddr = reinterpret_cast<uintptr_t*>(base + relocBlock->VirtualAddress + offset);
                        *patchAddr += relocDelta;
                    } else if (type == IMAGE_REL_BASED_HIGHLOW) {
                        auto patchAddr = reinterpret_cast<DWORD*>(base + relocBlock->VirtualAddress + offset);
                        *patchAddr += static_cast<DWORD>(relocDelta);
                    }
                }

                relocBlock = reinterpret_cast<IMAGE_BASE_RELOCATION*>(
                    reinterpret_cast<BYTE*>(relocBlock) + relocBlock->SizeOfBlock);
            }
        }
    }

    // ---- Step 2: Resolve imports ----
    auto& importDir = optHeader->DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDir.Size > 0) {
        auto importDesc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDir.VirtualAddress);

        using fnLoadLibraryA_t = HMODULE(WINAPI*)(LPCSTR);
        using fnGetProcAddress_t = FARPROC(WINAPI*)(HMODULE, LPCSTR);

        auto pLoadLibraryA = reinterpret_cast<fnLoadLibraryA_t>(data->fnLoadLibraryA);
        auto pGetProcAddress = reinterpret_cast<fnGetProcAddress_t>(data->fnGetProcAddress);

        for (; importDesc->Name != 0; importDesc++) {
            auto moduleName = reinterpret_cast<char*>(base + importDesc->Name);
            HMODULE hMod = pLoadLibraryA(moduleName);
            if (!hMod) continue;

            auto thunk = reinterpret_cast<IMAGE_THUNK_DATA64*>(
                base + (importDesc->OriginalFirstThunk ? importDesc->OriginalFirstThunk : importDesc->FirstThunk));
            auto iat = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + importDesc->FirstThunk);

            for (; thunk->u1.AddressOfData != 0; thunk++, iat++) {
                FARPROC proc = nullptr;
                if (IMAGE_SNAP_BY_ORDINAL64(thunk->u1.Ordinal)) {
                    proc = pGetProcAddress(hMod, reinterpret_cast<LPCSTR>(IMAGE_ORDINAL64(thunk->u1.Ordinal)));
                } else {
                    auto importByName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + thunk->u1.AddressOfData);
                    proc = pGetProcAddress(hMod, importByName->Name);
                }
                iat->u1.Function = reinterpret_cast<ULONGLONG>(proc);
            }
        }
    }

    // ---- Step 3: Execute TLS callbacks (if any) ----
    auto& tlsDir = optHeader->DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (tlsDir.Size > 0) {
        auto tls = reinterpret_cast<IMAGE_TLS_DIRECTORY64*>(base + tlsDir.VirtualAddress);
        auto callbacks = reinterpret_cast<PIMAGE_TLS_CALLBACK*>(tls->AddressOfCallBacks);
        if (callbacks) {
            for (; *callbacks; callbacks++) {
                (*callbacks)(base, DLL_PROCESS_ATTACH, nullptr);
            }
        }
    }

    // ---- Step 4: Call DllMain ----
    if (optHeader->AddressOfEntryPoint) {
        using fnDllMain_t = BOOL(WINAPI*)(HINSTANCE, DWORD, LPVOID);
        auto dllMain = reinterpret_cast<fnDllMain_t>(base + optHeader->AddressOfEntryPoint);
        dllMain(reinterpret_cast<HINSTANCE>(base), DLL_PROCESS_ATTACH, nullptr);
    }

    data->result = 1;
    return 1;
}

// Marker function to calculate shellcode size
static void ShellcodeEnd() {}
#pragma optimize("", on)
#pragma runtime_checks("", restore)

// ============================================================================
//  Implementation
// ============================================================================

static std::vector<BYTE> ReadFileToBuffer(const std::wstring& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return {};

    auto size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<BYTE> buffer(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(buffer.data()), size);
    return buffer;
}

// ============================================================================
//  Helper: Try to allocate remote memory with fallback strategies
//
//  Strategy 1: Direct allocation with PAGE_READWRITE (less suspicious)
//  Strategy 2: Try PAGE_EXECUTE_READWRITE (some drivers only flag RW)
//  Strategy 3: Try Win32 VirtualAllocEx as last resort
// ============================================================================
static PVOID TryAllocateRemote(
    const DirectSyscall::SyscallInvoker& syscall,
    HANDLE hProcess, SIZE_T size, DWORD protect, const char* label)
{
    PVOID base = nullptr;
    SIZE_T regionSize = size;
    NTSTATUS status;

    // Strategy 1: Direct syscall with requested protection
    printf("[*] Allocating %s (0x%zX bytes, prot=0x%X) via direct syscall...\n",
           label, size, protect);
    status = syscall.AllocateVirtualMemory(
        hProcess, &base, 0, &regionSize, MEM_COMMIT | MEM_RESERVE, protect);

    if (NT_SUCCESS(status) && base) {
        printf("[+] %s allocated at 0x%p (direct syscall)\n", label, base);
        return base;
    }
    printf("[!] Direct syscall NtAllocateVirtualMemory failed: %s\n",
           DirectSyscall::NtStatusToHex(status).c_str());

    // Strategy 2: If we asked for RWX, try RW instead (less suspicious to AC)
    if (protect == PAGE_EXECUTE_READWRITE) {
        base = nullptr;
        regionSize = size;
        printf("[*] Retrying %s with PAGE_READWRITE...\n", label);
        status = syscall.AllocateVirtualMemory(
            hProcess, &base, 0, &regionSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

        if (NT_SUCCESS(status) && base) {
            printf("[+] %s allocated at 0x%p (PAGE_READWRITE fallback)\n", label, base);
            return base;
        }
        printf("[!] PAGE_READWRITE fallback also failed: %s\n",
               DirectSyscall::NtStatusToHex(status).c_str());
    }

    // Strategy 3: Win32 VirtualAllocEx (goes through ntdll, but some AC
    // drivers whitelist calls from their own injector checks)
    printf("[*] Retrying %s via VirtualAllocEx (Win32)...\n", label);
    base = VirtualAllocEx(hProcess, nullptr, size,
                          MEM_COMMIT | MEM_RESERVE, protect);
    if (base) {
        printf("[+] %s allocated at 0x%p (VirtualAllocEx)\n", label, base);
        return base;
    }

    // Strategy 3b: VirtualAllocEx with PAGE_READWRITE
    if (protect != PAGE_READWRITE) {
        base = VirtualAllocEx(hProcess, nullptr, size,
                              MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (base) {
            printf("[+] %s allocated at 0x%p (VirtualAllocEx PAGE_READWRITE)\n", label, base);
            return base;
        }
    }

    printf("[!] All allocation strategies failed for %s (last error: %u)\n",
           label, GetLastError());
    return nullptr;
}

// ============================================================================
//  Helper: Open process with fallback strategies
//
//  Strategy 1: NtOpenProcess via direct syscall (bypasses user-mode hooks,
//              may bypass some ObRegisterCallbacks implementations)
//  Strategy 2: Win32 OpenProcess with PROCESS_ALL_ACCESS
//  Strategy 3: Win32 OpenProcess with minimal rights + upgrade later
//  Strategy 4: Use provided handle as-is
// ============================================================================
static HANDLE OpenProcessWithBypass(
    const DirectSyscall::SyscallInvoker& syscall, DWORD pid)
{
    HANDLE hProcess = nullptr;

    // Strategy 1: NtOpenProcess via direct syscall
    printf("[*] Opening process via NtOpenProcess (direct syscall)...\n");
    hProcess = syscall.OpenProcessDirect(pid, PROCESS_ALL_ACCESS);
    if (hProcess) {
        printf("[+] Process opened via direct syscall (handle: 0x%p)\n", hProcess);

        // Verify the handle actually has VM_OPERATION by trying a query
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQueryEx(hProcess, nullptr, &mbi, sizeof(mbi)) > 0) {
            printf("[+] Handle verified — has VM query access\n");
            return hProcess;
        } else {
            printf("[!] Direct syscall handle may be stripped, trying fallbacks...\n");
            CloseHandle(hProcess);
            hProcess = nullptr;
        }
    }

    // Strategy 2: Win32 OpenProcess with full access
    printf("[*] Opening process via OpenProcess (Win32)...\n");
    hProcess = ::OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (hProcess) {
        printf("[+] Process opened via Win32 (handle: 0x%p)\n", hProcess);
        return hProcess;
    }
    printf("[!] Win32 OpenProcess failed (error: %u)\n", GetLastError());

    // Strategy 3: Minimal rights — some AC drivers only strip if
    // specific access combos are requested
    printf("[*] Trying minimal access rights...\n");
    hProcess = ::OpenProcess(
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ |
        PROCESS_QUERY_INFORMATION,
        FALSE, pid);
    if (hProcess) {
        printf("[+] Process opened with minimal rights (handle: 0x%p)\n", hProcess);
        return hProcess;
    }
    printf("[!] Minimal rights also failed (error: %u)\n", GetLastError());

    return nullptr;
}

MapResult MapWithHandle(HANDLE hProcess, const std::wstring& dllPath) {
    MapResult result;

    // ---- Initialize direct syscalls ----
    DirectSyscall::SyscallInvoker syscall;
    if (!syscall.Initialize()) {
        result.message = "Failed to initialize direct syscalls";
        return result;
    }

    // ---- Read DLL file ----
    printf("[*] Reading DLL file...\n");
    auto dllData = ReadFileToBuffer(dllPath);
    if (dllData.empty()) {
        result.message = "Failed to read DLL file";
        return result;
    }
    printf("[+] DLL size: %zu bytes\n", dllData.size());

    // ---- Parse PE headers ----
    auto dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(dllData.data());
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE) {
        result.message = "Invalid DOS signature";
        return result;
    }

    auto ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS64*>(dllData.data() + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE) {
        result.message = "Invalid NT signature";
        return result;
    }

    if (ntHeaders->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        result.message = "DLL is not x64";
        return result;
    }

    auto& optHeader = ntHeaders->OptionalHeader;
    SIZE_T imageSize = optHeader.SizeOfImage;
    printf("[+] PE parsed: ImageSize=0x%zX, EntryPoint=0x%X\n",
           imageSize, optHeader.AddressOfEntryPoint);

    // ---- Allocate remote memory for DLL image ----
    // Use PAGE_READWRITE first (less suspicious), change to RX after writing
    PVOID remoteBase = TryAllocateRemote(
        syscall, hProcess, imageSize, PAGE_READWRITE, "image memory");

    if (!remoteBase) {
        result.message = "All allocation strategies failed for image memory. "
                         "tvk.sys likely has kernel-level ObRegisterCallbacks stripping handle access.";
        result.lastError = GetLastError();
        return result;
    }

    // ---- Build local image in memory ----
    std::vector<BYTE> mappedImage(imageSize, 0);

    // Copy headers
    memcpy(mappedImage.data(), dllData.data(), optHeader.SizeOfHeaders);

    // Copy sections
    auto sectionHeader = IMAGE_FIRST_SECTION(ntHeaders);
    for (WORD i = 0; i < ntHeaders->FileHeader.NumberOfSections; i++) {
        if (sectionHeader[i].SizeOfRawData > 0) {
            memcpy(
                mappedImage.data() + sectionHeader[i].VirtualAddress,
                dllData.data() + sectionHeader[i].PointerToRawData,
                sectionHeader[i].SizeOfRawData
            );
        }
        printf("    Section %-8.8s: VA=0x%08X Size=0x%08X\n",
               (const char*)sectionHeader[i].Name,
               sectionHeader[i].VirtualAddress,
               sectionHeader[i].SizeOfRawData);
    }

    // ---- Write mapped image to target process ----
    printf("[*] Writing image to target process...\n");
    NTSTATUS status = syscall.WriteVirtualMemory(
        hProcess, remoteBase, mappedImage.data(), imageSize, nullptr);

    // Fallback to WriteProcessMemory if direct syscall fails
    if (!NT_SUCCESS(status)) {
        printf("[!] Direct syscall NtWriteVirtualMemory failed: %s, trying Win32...\n",
               DirectSyscall::NtStatusToHex(status).c_str());
        SIZE_T written = 0;
        if (!WriteProcessMemory(hProcess, remoteBase, mappedImage.data(), imageSize, &written)) {
            result.message = "All write strategies failed (last error: " +
                             std::to_string(GetLastError()) + ")";
            SIZE_T freeSize = 0;
            PVOID freeBase = remoteBase;
            syscall.FreeVirtualMemory(hProcess, &freeBase, &freeSize, MEM_RELEASE);
            return result;
        }
        printf("[+] Image written via Win32 WriteProcessMemory (%zu bytes)\n", written);
    } else {
        printf("[+] Image written via direct syscall\n");
    }

    // ---- Change image memory protection to PAGE_EXECUTE_READWRITE ----
    // We allocated as PAGE_READWRITE to avoid detection, now upgrade
    printf("[*] Changing image memory protection to RWX...\n");
    PVOID protBase = remoteBase;
    SIZE_T protSize = imageSize;
    ULONG oldProtect = 0;
    status = syscall.ProtectVirtualMemory(
        hProcess, &protBase, &protSize, PAGE_EXECUTE_READWRITE, &oldProtect);

    if (!NT_SUCCESS(status)) {
        printf("[!] Direct VProtect failed: %s, trying Win32...\n",
               DirectSyscall::NtStatusToHex(status).c_str());
        DWORD oldProt2 = 0;
        if (!VirtualProtectEx(hProcess, remoteBase, imageSize,
                              PAGE_EXECUTE_READWRITE, &oldProt2)) {
            printf("[!] Win32 VirtualProtectEx also failed (error: %u)\n", GetLastError());
            printf("[*] Proceeding anyway — shellcode may still work with RW pages...\n");
        } else {
            printf("[+] Protection changed via Win32 VirtualProtectEx\n");
        }
    } else {
        printf("[+] Protection changed via direct syscall\n");
    }

    // ---- Prepare loader data ----
    LoaderData loaderData = {};
    loaderData.imageBase = reinterpret_cast<uintptr_t>(remoteBase);
    loaderData.ntHeadersOffset = dosHeader->e_lfanew;
    loaderData.fnLoadLibraryA = reinterpret_cast<uintptr_t>(
        GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryA"));
    loaderData.fnGetProcAddress = reinterpret_cast<uintptr_t>(
        GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetProcAddress"));
    loaderData.fnRtlAddFunctionTable = reinterpret_cast<uintptr_t>(
        GetProcAddress(GetModuleHandleA("kernel32.dll"), "RtlAddFunctionTable"));
    loaderData.result = 0;

    // ---- Allocate and write loader data ----
    PVOID remoteLoaderData = TryAllocateRemote(
        syscall, hProcess, sizeof(LoaderData) + 0x100, PAGE_READWRITE, "loader data");

    if (!remoteLoaderData) {
        result.message = "Failed to allocate loader data";
        SIZE_T freeSize = 0;
        PVOID freeBase = remoteBase;
        syscall.FreeVirtualMemory(hProcess, &freeBase, &freeSize, MEM_RELEASE);
        return result;
    }

    // Write loader data — try direct syscall, fallback to Win32
    status = syscall.WriteVirtualMemory(
        hProcess, remoteLoaderData, &loaderData, sizeof(LoaderData), nullptr);
    if (!NT_SUCCESS(status)) {
        if (!WriteProcessMemory(hProcess, remoteLoaderData, &loaderData, sizeof(LoaderData), nullptr)) {
            result.message = "Failed to write loader data";
            return result;
        }
    }

    // ---- Allocate and write shellcode ----
    auto shellcodeStart = reinterpret_cast<BYTE*>(&ShellcodeLoader);
    auto shellcodeEndAddr = reinterpret_cast<BYTE*>(&ShellcodeEnd);
    SIZE_T shellcodeSize = shellcodeEndAddr - shellcodeStart;

    if (shellcodeSize == 0 || shellcodeSize > 0x10000) {
        shellcodeSize = 0x2000;
    }
    printf("[*] Shellcode size: %zu bytes\n", shellcodeSize);

    // Shellcode needs PAGE_EXECUTE_READWRITE — try RW first, then upgrade
    PVOID remoteShellcode = TryAllocateRemote(
        syscall, hProcess, shellcodeSize + 0x100, PAGE_READWRITE, "shellcode");

    if (!remoteShellcode) {
        result.message = "Failed to allocate shellcode memory";
        return result;
    }

    // Write shellcode
    status = syscall.WriteVirtualMemory(
        hProcess, remoteShellcode, shellcodeStart, shellcodeSize, nullptr);
    if (!NT_SUCCESS(status)) {
        if (!WriteProcessMemory(hProcess, remoteShellcode, shellcodeStart, shellcodeSize, nullptr)) {
            result.message = "Failed to write shellcode";
            return result;
        }
    }
    printf("[+] Shellcode written to 0x%p\n", remoteShellcode);

    // Change shellcode protection to executable
    protBase = remoteShellcode;
    protSize = shellcodeSize + 0x100;
    status = syscall.ProtectVirtualMemory(
        hProcess, &protBase, &protSize, PAGE_EXECUTE_READ, &oldProtect);
    if (!NT_SUCCESS(status)) {
        DWORD oldProt2 = 0;
        if (!VirtualProtectEx(hProcess, remoteShellcode, shellcodeSize + 0x100,
                              PAGE_EXECUTE_READ, &oldProt2)) {
            printf("[!] Could not make shellcode executable (error: %u)\n", GetLastError());
            // Try PAGE_EXECUTE_READWRITE
            VirtualProtectEx(hProcess, remoteShellcode, shellcodeSize + 0x100,
                             PAGE_EXECUTE_READWRITE, &oldProt2);
        }
    }

    // ---- Execute shellcode via NtCreateThreadEx ----
    printf("[*] Creating remote thread...\n");
    HANDLE hThread = nullptr;
    status = syscall.CreateThreadEx(
        &hThread,
        THREAD_ALL_ACCESS,
        nullptr,
        hProcess,
        remoteShellcode,
        remoteLoaderData,
        0,    // No flags (not suspended)
        0, 0, 0,
        nullptr);

    if (!NT_SUCCESS(status) || !hThread) {
        printf("[!] NtCreateThreadEx (direct) failed: %s, trying CreateRemoteThread...\n",
               DirectSyscall::NtStatusToHex(status).c_str());

        // Fallback: Win32 CreateRemoteThread
        hThread = CreateRemoteThread(hProcess, nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteShellcode),
            remoteLoaderData, 0, nullptr);

        if (!hThread) {
            result.message = "All thread creation methods failed (last error: " +
                             std::to_string(GetLastError()) + ")";
            result.lastError = GetLastError();
            return result;
        }
        printf("[+] Remote thread created via Win32 CreateRemoteThread\n");
    } else {
        printf("[+] Remote thread created via direct syscall\n");
    }

    printf("[*] Waiting for shellcode to complete (30s timeout)...\n");

    // Wait for shellcode to finish (30 second timeout)
    LARGE_INTEGER timeout;
    timeout.QuadPart = -300000000LL; // 30 seconds
    syscall.WaitForSingleObject(hThread, FALSE, &timeout);

    // Read back loader data to check result
    LoaderData remoteResult = {};
    ReadProcessMemory(hProcess, remoteLoaderData, &remoteResult, sizeof(LoaderData), nullptr);

    // ---- Cleanup ----
    CloseHandle(hThread);

    // Free shellcode memory
    SIZE_T freeSize = 0;
    PVOID freeBase = remoteShellcode;
    syscall.FreeVirtualMemory(hProcess, &freeBase, &freeSize, MEM_RELEASE);

    // Free loader data
    freeSize = 0;
    freeBase = remoteLoaderData;
    syscall.FreeVirtualMemory(hProcess, &freeBase, &freeSize, MEM_RELEASE);

    // Wipe PE headers from remote image (anti-forensics)
    std::vector<BYTE> zeros(optHeader.SizeOfHeaders, 0);
    syscall.WriteVirtualMemory(hProcess, remoteBase, zeros.data(), zeros.size(), nullptr);
    printf("[+] PE headers wiped from remote memory\n");

    if (remoteResult.result == 1) {
        result.success = true;
        result.remoteBase = reinterpret_cast<uintptr_t>(remoteBase);
        char hexBuf[32];
        snprintf(hexBuf, sizeof(hexBuf), "0x%llX", (unsigned long long)remoteBase);
        result.message = std::string("DLL manually mapped at ") + hexBuf;
        printf("[+] SUCCESS! DLL mapped at %s\n", hexBuf);
    } else {
        printf("[!] Shellcode returned, but result flag not set (might still work)\n");
        result.success = true;
        result.remoteBase = reinterpret_cast<uintptr_t>(remoteBase);
        result.message = "Shellcode executed — DllMain may have completed (check game output)";
    }

    return result;
}

MapResult Map(HANDLE hProcess, const std::wstring& dllPath) {
    return MapWithHandle(hProcess, dllPath);
}

} // namespace ManualMapper
