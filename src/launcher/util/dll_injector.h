#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <string>

namespace DllInjector {

struct InjectionResult {
    bool success = false;
    std::string message;
    DWORD lastError = 0;
};

// Legacy LoadLibrary injection (blocked by tvk.sys)
InjectionResult Inject(DWORD pid, const std::wstring& dllPath);

// Manual mapping injection (bypasses user-mode hooks via direct syscalls)
InjectionResult ManualMapInject(DWORD pid, const std::wstring& dllPath);

} // namespace DllInjector
