#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <string>
#include <thread>
#include <atomic>
#include "log_buffer.h"

class ProcessLauncher {
public:
    ProcessLauncher(const std::string& name, LogBuffer& log);
    ~ProcessLauncher();

    ProcessLauncher(const ProcessLauncher&) = delete;
    ProcessLauncher& operator=(const ProcessLauncher&) = delete;

    bool Start(const std::wstring& exePath, const std::wstring& args = L"",
               const std::wstring& workingDir = L"");
    void Stop();
    bool IsRunning() const;
    DWORD GetPid() const { return m_pid; }
    const std::string& GetName() const { return m_name; }

    // Get last output line (for quick status)
    std::string GetLastLine() const;

private:
    void ReaderThreadFunc(HANDLE pipeRead);

    std::string m_name;
    LogBuffer& m_log;

    HANDLE m_process = nullptr;
    HANDLE m_thread = nullptr;
    DWORD m_pid = 0;

    std::thread m_readerThread;
    std::atomic<bool> m_running{false};

    mutable std::mutex m_lastLineMutex;
    std::string m_lastLine;
};
