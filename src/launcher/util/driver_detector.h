#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <string>

namespace DriverDetector {

// Check if a kernel driver/service is running via Service Control Manager
inline bool IsDriverLoaded(const wchar_t* serviceName) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) return false;

    SC_HANDLE svc = OpenServiceW(scm, serviceName, SERVICE_QUERY_STATUS);
    if (!svc) {
        CloseServiceHandle(scm);
        return false;
    }

    SERVICE_STATUS status{};
    bool running = false;
    if (QueryServiceStatus(svc, &status)) {
        running = (status.dwCurrentState == SERVICE_RUNNING);
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return running;
}

// Check specifically for tvk.sys (Dark and Darker anti-cheat)
inline bool IsTvkDriverLoaded() {
    return IsDriverLoaded(L"tvk");
}

} // namespace DriverDetector
