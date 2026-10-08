#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <TlHelp32.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "core/manual_mapper.h"
#include "core/syscalls.h"

static DWORD FindProcess(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);

    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static bool DoLoadLibraryInject(DWORD pid, const wchar_t* dllPath) {
    printf("[*] Using LoadLibrary injection (legacy)...\n");
    printf("[*] Target PID: %u\n", pid);

    HANDLE hProcess = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);

    if (!hProcess) {
        printf("[!] OpenProcess failed (error %u). Run as Administrator!\n", GetLastError());
        return false;
    }

    size_t pathSize = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    LPVOID remoteMem = VirtualAllocEx(hProcess, nullptr, pathSize,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        printf("[!] VirtualAllocEx failed (error %u)\n", GetLastError());
        CloseHandle(hProcess);
        return false;
    }

    WriteProcessMemory(hProcess, remoteMem, dllPath, pathSize, nullptr);

    auto loadLibAddr = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));

    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0,
                                         loadLibAddr, remoteMem, 0, nullptr);
    if (!hThread) {
        printf("[!] CreateRemoteThread failed (error %u)\n", GetLastError());
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    WaitForSingleObject(hThread, 30000);
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);

    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hThread);
    CloseHandle(hProcess);

    if (exitCode == 0) {
        printf("[!] LoadLibraryW returned NULL\n");
        return false;
    }
    printf("[+] SUCCESS via LoadLibrary! Module: 0x%X\n", exitCode);
    return true;
}

static bool DoManualMapInject(DWORD pid, const wchar_t* dllPath) {
    printf("[*] Using Manual Mapping injection (direct syscalls)...\n");
    printf("[*] Target PID: %u\n\n", pid);

    // Initialize direct syscall stubs for NtOpenProcess bypass
    DirectSyscall::SyscallInvoker syscallHelper;
    if (!syscallHelper.Initialize()) {
        printf("[!] Failed to initialize syscall stubs\n");
        return false;
    }

    HANDLE hProcess = nullptr;

    // Strategy 1: NtOpenProcess via direct syscall
    // This bypasses user-mode hooks. ObRegisterCallbacks may or may not
    // strip the handle — depends on how the AC driver filters.
    printf("\n[*] Strategy 1: NtOpenProcess via direct syscall...\n");
    hProcess = syscallHelper.OpenProcessDirect(pid, PROCESS_ALL_ACCESS);
    if (hProcess) {
        printf("[+] Got handle via direct syscall: 0x%p\n", hProcess);

        // Verify the handle works by trying VirtualQueryEx
        MEMORY_BASIC_INFORMATION mbi = {};
        SIZE_T queryResult = VirtualQueryEx(hProcess, (LPCVOID)0x10000, &mbi, sizeof(mbi));
        if (queryResult > 0) {
            printf("[+] Handle verified — VirtualQueryEx succeeded\n");
        } else {
            printf("[!] Handle appears stripped (VirtualQueryEx failed, error: %u)\n",
                   GetLastError());
            // Don't give up — the handle might still work for allocation
            printf("[*] Proceeding with possibly-stripped handle...\n");
        }
    } else {
        printf("[!] NtOpenProcess direct syscall failed\n");
    }

    // Strategy 2: Win32 OpenProcess with PROCESS_ALL_ACCESS
    if (!hProcess) {
        printf("[*] Strategy 2: Win32 OpenProcess (PROCESS_ALL_ACCESS)...\n");
        hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
        if (hProcess) {
            printf("[+] Got handle via Win32: 0x%p\n", hProcess);
        } else {
            printf("[!] Win32 OpenProcess failed (error: %u)\n", GetLastError());
        }
    }

    // Strategy 3: Minimal rights
    if (!hProcess) {
        printf("[*] Strategy 3: Minimal rights...\n");
        hProcess = OpenProcess(
            PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ |
            PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION,
            FALSE, pid);
        if (hProcess) {
            printf("[+] Got handle with minimal rights: 0x%p\n", hProcess);
        } else {
            printf("[!] Minimal rights also failed (error: %u)\n", GetLastError());
        }
    }

    if (!hProcess) {
        printf("\n[!] All process open strategies failed! Run as Administrator!\n");
        return false;
    }

    printf("\n");
    auto result = ManualMapper::Map(hProcess, dllPath);
    CloseHandle(hProcess);

    if (result.success) {
        printf("\n[+] %s\n", result.message.c_str());
        return true;
    } else {
        printf("\n[!] %s\n", result.message.c_str());
        return false;
    }
}

int main(int argc, char* argv[]) {
    printf("=== DAD Research Tool - DLL Injector ===\n\n");

    // Parse flags
    bool useLoadLib = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--loadlib") == 0 || strcmp(argv[i], "-l") == 0) {
            useLoadLib = true;
        }
    }

    DWORD pid = 0;
    wchar_t dllPath[MAX_PATH] = {};

    // Check for positional args: inject.exe [PID] [DLL_PATH]
    int posArgs = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue; // Skip flags
        if (posArgs == 0) {
            pid = static_cast<DWORD>(atoi(argv[i]));
            posArgs++;
        } else if (posArgs == 1) {
            MultiByteToWideChar(CP_ACP, 0, argv[i], -1, dllPath, MAX_PATH);
            posArgs++;
        }
    }

    if (posArgs < 2) {
        // Interactive mode
        printf("[*] Interactive mode (use --loadlib for legacy injection)\n\n");

        printf("[*] Searching for DungeonCrawler.exe...\n");
        pid = FindProcess(L"DungeonCrawler.exe");
        if (pid == 0) {
            printf("[!] DungeonCrawler.exe not found! Is the game running?\n");
            printf("\nPress Enter to exit...");
            getchar();
            return 1;
        }
        printf("[+] Found DungeonCrawler.exe (PID: %u)\n\n", pid);

        // Auto-detect DLL path
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);

        wchar_t* lastSlash = wcsrchr(exePath, L'\\');
        if (lastSlash) *lastSlash = 0;
        lastSlash = wcsrchr(exePath, L'\\');
        if (lastSlash) *lastSlash = 0;
        lastSlash = wcsrchr(exePath, L'\\');
        if (lastSlash) *lastSlash = 0;

        swprintf_s(dllPath, L"%s\\tools\\dumper7\\build\\bin\\Release\\Dumper-7.dll", exePath);

        if (GetFileAttributesW(dllPath) == INVALID_FILE_ATTRIBUTES) {
            printf("[!] Dumper-7.dll not found at default path.\n");
            printf("[*] Expected: %ls\n", dllPath);
            printf("\nPress Enter to exit...");
            getchar();
            return 1;
        }
        printf("[+] Found Dumper-7.dll\n\n");
    }

    printf("[*] Method: %s\n\n", useLoadLib ? "LoadLibrary (legacy)" : "Manual Map (direct syscalls)");

    bool ok = false;
    if (useLoadLib) {
        ok = DoLoadLibraryInject(pid, dllPath);
    } else {
        ok = DoManualMapInject(pid, dllPath);
    }

    if (ok) {
        printf("\n[*] Waiting 15 seconds for Dumper-7 to generate output...\n");
        Sleep(15000);

        const wchar_t* checkPaths[] = {
            L"D:\\SteamLibrary\\steamapps\\common\\Dark and Darker\\Dumper-7",
            L"D:\\SteamLibrary\\steamapps\\common\\Dark and Darker\\DungeonCrawler\\Binaries\\Win64\\Dumper-7",
        };

        bool found = false;
        for (auto& path : checkPaths) {
            if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
                printf("[+] Dumper-7 output found at: %ls\n", path);
                found = true;
            }
        }
        if (!found) {
            printf("[*] No Dumper-7 output folder found yet. It may need more time.\n");
        }
    }

    printf("\n[*] Done.\n");
    printf("Press Enter to exit...");
    getchar();
    return 0;
}
