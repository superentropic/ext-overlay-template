#pragma once
// ============================================================================
//  Direct Syscall Wrappers — bypass user-mode NTDLL hooks
//
//  Technique: Read syscall numbers from NTDLL stubs at runtime (even hooked
//  stubs preserve the mov eax, <number> instruction). Then execute syscall
//  instruction via dynamically allocated executable shellcode.
//
//  This bypasses anti-cheat hooks on NtAllocateVirtualMemory,
//  NtWriteVirtualMemory, NtProtectVirtualMemory, NtCreateThreadEx, and
//  NtOpenProcess (to bypass ObRegisterCallbacks handle stripping).
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <cstdint>
#include <cstdio>

// NTDLL types not included in standard Windows headers
#ifndef NTSTATUS
typedef LONG NTSTATUS;
#endif

#ifndef NTAPI
#define NTAPI __stdcall
#endif

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

// NtOpenProcess requires these NTDLL structures
typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

typedef struct _OBJECT_ATTRIBUTES {
    ULONG           Length;
    HANDLE          RootDirectory;
    PUNICODE_STRING ObjectName;
    ULONG           Attributes;
    PVOID           SecurityDescriptor;
    PVOID           SecurityQualityOfService;
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;

typedef struct _CLIENT_ID {
    HANDLE UniqueProcess;
    HANDLE UniqueThread;
} CLIENT_ID, *PCLIENT_ID;

#ifndef InitializeObjectAttributes
#define InitializeObjectAttributes(p, n, a, r, s) { \
    (p)->Length = sizeof(OBJECT_ATTRIBUTES); \
    (p)->RootDirectory = r; \
    (p)->Attributes = a; \
    (p)->ObjectName = n; \
    (p)->SecurityDescriptor = s; \
    (p)->SecurityQualityOfService = nullptr; \
}
#endif

namespace DirectSyscall {

// ============================================================================
//  NTSTATUS formatting helper
// ============================================================================
inline std::string NtStatusToHex(NTSTATUS status) {
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned int>(status));
    return std::string(buf);
}

// ============================================================================
//  Runtime syscall number extraction
// ============================================================================
inline DWORD GetSyscallNumber(const char* functionName) {
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (!ntdll) return 0;

    auto fn = reinterpret_cast<BYTE*>(GetProcAddress(ntdll, functionName));
    if (!fn) return 0;

    // Standard NTDLL stub pattern (unhooked):
    //   4C 8B D1        mov r10, rcx
    //   B8 XX XX XX XX  mov eax, <syscall_number>
    //   0F 05           syscall
    if (fn[0] == 0x4C && fn[1] == 0x8B && fn[2] == 0xD1 && fn[3] == 0xB8) {
        return *reinterpret_cast<DWORD*>(fn + 4);
    }

    // Hooked stub — the hook typically replaces the first bytes with a jmp,
    // but the mov eax, <number> is usually preserved a few bytes in.
    // Search forward for the B8 pattern.
    for (int i = 0; i < 64; i++) {
        if (fn[i] == 0x0F && fn[i + 1] == 0x05) {
            // Found syscall instruction, look backwards for mov eax
            for (int j = i - 1; j >= 0; j--) {
                if (fn[j] == 0xB8) {
                    return *reinterpret_cast<DWORD*>(fn + j + 1);
                }
            }
        }
    }

    return 0; // Failed to resolve
}

// ============================================================================
//  Syscall stub — x64 shellcode that performs the syscall
//
//  MSVC x64 doesn't support inline asm. We build a tiny executable stub
//  at runtime: mov r10, rcx / mov eax, <num> / syscall / ret
//
//  This is 15 bytes of position-independent code.
// ============================================================================

// Shellcode template:
//   4C 8B D1              mov r10, rcx     ; Windows syscall convention
//   B8 XX XX XX XX        mov eax, <num>   ; syscall number (patched)
//   0F 05                 syscall
//   C3                    ret
static constexpr BYTE SYSCALL_STUB[] = {
    0x4C, 0x8B, 0xD1,                      // mov r10, rcx
    0xB8, 0x00, 0x00, 0x00, 0x00,          // mov eax, <placeholder>
    0x0F, 0x05,                             // syscall
    0xC3                                    // ret
};
static constexpr size_t SYSCALL_STUB_SIZE = sizeof(SYSCALL_STUB);
static constexpr size_t SYSCALL_NUM_OFFSET = 4; // Offset of the syscall number in stub

// ============================================================================
//  SyscallInvoker — allocates executable memory for a syscall stub
// ============================================================================
class SyscallInvoker {
public:
    static constexpr int NUM_STUBS = 8;

    SyscallInvoker() = default;
    ~SyscallInvoker() { Cleanup(); }

    bool Initialize() {
        // Resolve all syscall numbers
        m_numAllocate   = GetSyscallNumber("NtAllocateVirtualMemory");
        m_numWrite      = GetSyscallNumber("NtWriteVirtualMemory");
        m_numProtect    = GetSyscallNumber("NtProtectVirtualMemory");
        m_numThread     = GetSyscallNumber("NtCreateThreadEx");
        m_numFree       = GetSyscallNumber("NtFreeVirtualMemory");
        m_numWait       = GetSyscallNumber("NtWaitForSingleObject");
        m_numOpenProc   = GetSyscallNumber("NtOpenProcess");
        m_numDupObject  = GetSyscallNumber("NtDuplicateObject");

        if (!m_numAllocate || !m_numWrite || !m_numProtect || !m_numThread || !m_numFree) {
            printf("[!] Failed to resolve core syscall numbers\n");
            printf("    NtAllocateVirtualMemory: 0x%X\n", m_numAllocate);
            printf("    NtWriteVirtualMemory:    0x%X\n", m_numWrite);
            printf("    NtProtectVirtualMemory:  0x%X\n", m_numProtect);
            printf("    NtCreateThreadEx:        0x%X\n", m_numThread);
            printf("    NtFreeVirtualMemory:     0x%X\n", m_numFree);
            return false;
        }

        // Allocate executable memory for stubs
        m_stubMem = static_cast<BYTE*>(VirtualAlloc(
            nullptr, SYSCALL_STUB_SIZE * NUM_STUBS, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!m_stubMem) return false;

        // Build stubs
        BuildStub(0, m_numAllocate);
        BuildStub(1, m_numWrite);
        BuildStub(2, m_numProtect);
        BuildStub(3, m_numThread);
        BuildStub(4, m_numFree);
        BuildStub(5, m_numWait);
        BuildStub(6, m_numOpenProc);
        BuildStub(7, m_numDupObject);

        printf("[+] Syscall stubs initialized\n");
        printf("    NtAllocateVirtualMemory: 0x%X\n", m_numAllocate);
        printf("    NtWriteVirtualMemory:    0x%X\n", m_numWrite);
        printf("    NtProtectVirtualMemory:  0x%X\n", m_numProtect);
        printf("    NtCreateThreadEx:        0x%X\n", m_numThread);
        printf("    NtFreeVirtualMemory:     0x%X\n", m_numFree);
        printf("    NtWaitForSingleObject:   0x%X\n", m_numWait);
        printf("    NtOpenProcess:           0x%X\n", m_numOpenProc);
        printf("    NtDuplicateObject:       0x%X\n", m_numDupObject);
        return true;
    }

    // =======================================================================
    //  Syscall wrappers — same signatures as NTDLL functions
    // =======================================================================

    // NtAllocateVirtualMemory(ProcessHandle, BaseAddress, ZeroBits, RegionSize, AllocType, Protect)
    NTSTATUS AllocateVirtualMemory(
        HANDLE processHandle, PVOID* baseAddress, ULONG_PTR zeroBits,
        PSIZE_T regionSize, ULONG allocationType, ULONG protect) const
    {
        using fn_t = NTSTATUS(NTAPI*)(HANDLE, PVOID*, ULONG_PTR, PSIZE_T, ULONG, ULONG);
        auto fn = reinterpret_cast<fn_t>(GetStub(0));
        return fn(processHandle, baseAddress, zeroBits, regionSize, allocationType, protect);
    }

    // NtWriteVirtualMemory(ProcessHandle, BaseAddress, Buffer, NumberOfBytes, NumberOfBytesWritten)
    NTSTATUS WriteVirtualMemory(
        HANDLE processHandle, PVOID baseAddress, PVOID buffer,
        SIZE_T numberOfBytes, PSIZE_T numberOfBytesWritten) const
    {
        using fn_t = NTSTATUS(NTAPI*)(HANDLE, PVOID, PVOID, SIZE_T, PSIZE_T);
        auto fn = reinterpret_cast<fn_t>(GetStub(1));
        return fn(processHandle, baseAddress, buffer, numberOfBytes, numberOfBytesWritten);
    }

    // NtProtectVirtualMemory(ProcessHandle, BaseAddress, RegionSize, NewProtect, OldProtect)
    NTSTATUS ProtectVirtualMemory(
        HANDLE processHandle, PVOID* baseAddress, PSIZE_T regionSize,
        ULONG newProtect, PULONG oldProtect) const
    {
        using fn_t = NTSTATUS(NTAPI*)(HANDLE, PVOID*, PSIZE_T, ULONG, PULONG);
        auto fn = reinterpret_cast<fn_t>(GetStub(2));
        return fn(processHandle, baseAddress, regionSize, newProtect, oldProtect);
    }

    // NtCreateThreadEx — undocumented, 11 parameters
    NTSTATUS CreateThreadEx(
        PHANDLE threadHandle, ACCESS_MASK desiredAccess, PVOID objectAttributes,
        HANDLE processHandle, PVOID startRoutine, PVOID argument,
        ULONG createFlags, SIZE_T zeroBits, SIZE_T stackSize,
        SIZE_T maximumStackSize, PVOID attributeList) const
    {
        using fn_t = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, PVOID, HANDLE,
            PVOID, PVOID, ULONG, SIZE_T, SIZE_T, SIZE_T, PVOID);
        auto fn = reinterpret_cast<fn_t>(GetStub(3));
        return fn(threadHandle, desiredAccess, objectAttributes, processHandle,
                  startRoutine, argument, createFlags, zeroBits, stackSize,
                  maximumStackSize, attributeList);
    }

    // NtFreeVirtualMemory(ProcessHandle, BaseAddress, RegionSize, FreeType)
    NTSTATUS FreeVirtualMemory(
        HANDLE processHandle, PVOID* baseAddress, PSIZE_T regionSize, ULONG freeType) const
    {
        using fn_t = NTSTATUS(NTAPI*)(HANDLE, PVOID*, PSIZE_T, ULONG);
        auto fn = reinterpret_cast<fn_t>(GetStub(4));
        return fn(processHandle, baseAddress, regionSize, freeType);
    }

    // NtWaitForSingleObject(Handle, Alertable, Timeout)
    NTSTATUS WaitForSingleObject(HANDLE handle, BOOLEAN alertable, PLARGE_INTEGER timeout) const {
        using fn_t = NTSTATUS(NTAPI*)(HANDLE, BOOLEAN, PLARGE_INTEGER);
        auto fn = reinterpret_cast<fn_t>(GetStub(5));
        return fn(handle, alertable, timeout);
    }

    // NtOpenProcess — bypass ObRegisterCallbacks handle stripping
    // The anti-cheat registers ObRegisterCallbacks which fires on EVERY
    // ObpPreOperationCallback. However, some drivers only filter based on
    // the calling process or access mask. Direct syscalling this gives us
    // a chance to get an unstripped handle.
    NTSTATUS OpenProcess(
        PHANDLE processHandle, ACCESS_MASK desiredAccess,
        POBJECT_ATTRIBUTES objectAttributes, PCLIENT_ID clientId) const
    {
        using fn_t = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PCLIENT_ID);
        auto fn = reinterpret_cast<fn_t>(GetStub(6));
        return fn(processHandle, desiredAccess, objectAttributes, clientId);
    }

    // NtDuplicateObject — duplicate handles between processes
    // Used to steal handles from system processes that may hold
    // unstripped handles to the game process
    NTSTATUS DuplicateObject(
        HANDLE sourceProcessHandle, HANDLE sourceHandle,
        HANDLE targetProcessHandle, PHANDLE targetHandle,
        ACCESS_MASK desiredAccess, ULONG handleAttributes,
        ULONG options) const
    {
        using fn_t = NTSTATUS(NTAPI*)(HANDLE, HANDLE, HANDLE, PHANDLE,
            ACCESS_MASK, ULONG, ULONG);
        auto fn = reinterpret_cast<fn_t>(GetStub(7));
        return fn(sourceProcessHandle, sourceHandle, targetProcessHandle,
                  targetHandle, desiredAccess, handleAttributes, options);
    }

    // ===== Convenience: Open process via direct syscall =====
    HANDLE OpenProcessDirect(DWORD pid, ACCESS_MASK access) const {
        HANDLE hProcess = nullptr;
        OBJECT_ATTRIBUTES oa;
        CLIENT_ID cid;

        InitializeObjectAttributes(&oa, nullptr, 0, nullptr, nullptr);
        cid.UniqueProcess = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(pid));
        cid.UniqueThread = nullptr;

        NTSTATUS status = OpenProcess(&hProcess, access, &oa, &cid);
        if (!NT_SUCCESS(status)) {
            printf("[!] NtOpenProcess (direct) failed: %s\n", NtStatusToHex(status).c_str());
            return nullptr;
        }
        return hProcess;
    }

private:
    void BuildStub(int index, DWORD syscallNum) {
        BYTE* stub = m_stubMem + (index * SYSCALL_STUB_SIZE);
        memcpy(stub, SYSCALL_STUB, SYSCALL_STUB_SIZE);
        *reinterpret_cast<DWORD*>(stub + SYSCALL_NUM_OFFSET) = syscallNum;
    }

    void* GetStub(int index) const {
        return m_stubMem + (index * SYSCALL_STUB_SIZE);
    }

    void Cleanup() {
        if (m_stubMem) {
            VirtualFree(m_stubMem, 0, MEM_RELEASE);
            m_stubMem = nullptr;
        }
    }

    BYTE* m_stubMem = nullptr;
    DWORD m_numAllocate = 0;
    DWORD m_numWrite = 0;
    DWORD m_numProtect = 0;
    DWORD m_numThread = 0;
    DWORD m_numFree = 0;
    DWORD m_numWait = 0;
    DWORD m_numOpenProc = 0;
    DWORD m_numDupObject = 0;
};

} // namespace DirectSyscall
