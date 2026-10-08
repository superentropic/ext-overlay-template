#include "dll_injector.h"
#include "core/manual_mapper.h"
#include "core/syscalls.h"
#include <spdlog/spdlog.h>

namespace DllInjector {

InjectionResult ManualMapInject(DWORD pid, const std::wstring& dllPath) {
    InjectionResult result;

    // Verify DLL exists
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        char narrowPath[MAX_PATH] = {};
        size_t converted = 0;
        wcstombs_s(&converted, narrowPath, dllPath.c_str(), sizeof(narrowPath) - 1);
        result.message = "DLL file not found: " + std::string(narrowPath);
        result.lastError = GetLastError();
        return result;
    }

    // Initialize syscalls for NtOpenProcess bypass
    DirectSyscall::SyscallInvoker syscallHelper;
    HANDLE hProcess = nullptr;

    if (syscallHelper.Initialize()) {
        // Try NtOpenProcess via direct syscall first (bypass ObRegisterCallbacks)
        hProcess = syscallHelper.OpenProcessDirect(pid, PROCESS_ALL_ACCESS);
        if (hProcess) {
            spdlog::info("Opened process via NtOpenProcess direct syscall");
        }
    }

    // Fallback to Win32 OpenProcess
    if (!hProcess) {
        hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    }

    if (!hProcess) {
        result.lastError = GetLastError();
        result.message = "Failed to open process (run as admin?). Error: " + std::to_string(result.lastError);
        return result;
    }

    spdlog::info("Manual mapping DLL into PID {}...", pid);

    // Perform manual mapping with direct syscalls
    auto mapResult = ManualMapper::MapWithHandle(hProcess, dllPath);
    CloseHandle(hProcess);

    result.success = mapResult.success;
    result.message = mapResult.message;
    result.lastError = mapResult.lastError;

    if (result.success) {
        spdlog::info("Manual map succeeded: {}", result.message);
    } else {
        spdlog::error("Manual map failed: {}", result.message);
    }

    return result;
}

InjectionResult Inject(DWORD pid, const std::wstring& dllPath) {
    InjectionResult result;

    // Verify DLL exists
    if (GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // Convert wide string to narrow for error message
        char narrowPath[MAX_PATH] = {};
        size_t converted = 0;
        wcstombs_s(&converted, narrowPath, dllPath.c_str(), sizeof(narrowPath) - 1);
        result.message = "DLL file not found: " + std::string(narrowPath);
        result.lastError = GetLastError();
        return result;
    }

    // Open target process with required access
    HANDLE hProcess = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE, pid);

    if (!hProcess) {
        result.lastError = GetLastError();
        result.message = "Failed to open process (run as admin?). Error: " + std::to_string(result.lastError);
        return result;
    }

    // Allocate memory in target for DLL path
    size_t pathSize = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remoteMem = VirtualAllocEx(hProcess, nullptr, pathSize,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteMem) {
        result.lastError = GetLastError();
        result.message = "VirtualAllocEx failed. Error: " + std::to_string(result.lastError);
        CloseHandle(hProcess);
        return result;
    }

    // Write DLL path to target memory
    if (!WriteProcessMemory(hProcess, remoteMem, dllPath.c_str(), pathSize, nullptr)) {
        result.lastError = GetLastError();
        result.message = "WriteProcessMemory failed. Error: " + std::to_string(result.lastError);
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return result;
    }

    // Get LoadLibraryW address
    auto loadLibAddr = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));

    if (!loadLibAddr) {
        result.message = "Failed to get LoadLibraryW address";
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return result;
    }

    // Create remote thread calling LoadLibraryW(dllPath)
    HANDLE hThread = CreateRemoteThread(hProcess, nullptr, 0,
                                         loadLibAddr, remoteMem, 0, nullptr);
    if (!hThread) {
        result.lastError = GetLastError();
        result.message = "CreateRemoteThread failed (anti-cheat blocking?). Error: " + std::to_string(result.lastError);
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return result;
    }

    // Wait for thread to complete (10 second timeout)
    DWORD waitResult = WaitForSingleObject(hThread, 10000);
    if (waitResult == WAIT_TIMEOUT) {
        result.message = "Remote thread timed out (10s) - DLL may still be loading";
        CloseHandle(hThread);
        VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return result;
    }

    // Check LoadLibrary return value
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);

    // Cleanup
    VirtualFreeEx(hProcess, remoteMem, 0, MEM_RELEASE);
    CloseHandle(hThread);
    CloseHandle(hProcess);

    if (exitCode == 0) {
        result.message = "LoadLibraryW returned NULL - DLL failed to load in target process";
    } else {
        result.success = true;
        result.message = "DLL injected successfully (module base: 0x" +
                         std::to_string(exitCode) + ")";
    }

    return result;
}

} // namespace DllInjector
