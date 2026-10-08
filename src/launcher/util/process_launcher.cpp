#include "process_launcher.h"
#include <spdlog/spdlog.h>
#include <algorithm>

ProcessLauncher::ProcessLauncher(const std::string& name, LogBuffer& log)
    : m_name(name), m_log(log) {}

ProcessLauncher::~ProcessLauncher() {
    Stop();
}

bool ProcessLauncher::Start(const std::wstring& exePath, const std::wstring& args,
                            const std::wstring& workingDir) {
    if (IsRunning()) {
        m_log.Push("Process '" + m_name + "' is already running", spdlog::level::warn);
        return false;
    }

    // Stop any previous instance
    Stop();

    // Create pipe for stdout/stderr capture
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE pipeRead = nullptr;
    HANDLE pipeWrite = nullptr;
    if (!CreatePipe(&pipeRead, &pipeWrite, &sa, 0)) {
        m_log.Push("Failed to create pipe for " + m_name, spdlog::level::err);
        return false;
    }

    // Don't let child inherit the read end
    SetHandleInformation(pipeRead, HANDLE_FLAG_INHERIT, 0);

    // Setup startup info with redirected stdout/stderr
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = pipeWrite;
    si.hStdError = pipeWrite;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};

    // Build command line
    std::wstring cmdLine = L"\"" + exePath + L"\"";
    if (!args.empty()) {
        cmdLine += L" " + args;
    }

    // Working directory
    const wchar_t* workDir = workingDir.empty() ? nullptr : workingDir.c_str();

    // Create process
    BOOL created = CreateProcessW(
        nullptr,
        cmdLine.data(),
        nullptr, nullptr,
        TRUE,  // inherit handles
        CREATE_NO_WINDOW,
        nullptr,
        workDir,
        &si, &pi
    );

    // Close write end in parent - child has its own copy
    CloseHandle(pipeWrite);

    if (!created) {
        DWORD err = GetLastError();
        m_log.Push("Failed to start " + m_name + ". Error: " + std::to_string(err),
                   spdlog::level::err);
        CloseHandle(pipeRead);
        return false;
    }

    m_process = pi.hProcess;
    m_thread = pi.hThread;
    m_pid = pi.dwProcessId;
    m_running = true;

    m_log.Push("Started " + m_name + " (PID: " + std::to_string(m_pid) + ")",
               spdlog::level::info);

    // Start reader thread
    m_readerThread = std::thread(&ProcessLauncher::ReaderThreadFunc, this, pipeRead);

    return true;
}

void ProcessLauncher::Stop() {
    if (m_process) {
        if (IsRunning()) {
            TerminateProcess(m_process, 0);
            WaitForSingleObject(m_process, 3000);
            m_log.Push("Stopped " + m_name, spdlog::level::info);
        }
    }

    m_running = false;

    if (m_readerThread.joinable()) {
        m_readerThread.join();
    }

    if (m_process) {
        CloseHandle(m_process);
        m_process = nullptr;
    }
    if (m_thread) {
        CloseHandle(m_thread);
        m_thread = nullptr;
    }
    m_pid = 0;
}

bool ProcessLauncher::IsRunning() const {
    if (!m_process) return false;
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(m_process, &exitCode)) return false;
    return exitCode == STILL_ACTIVE;
}

std::string ProcessLauncher::GetLastLine() const {
    std::lock_guard<std::mutex> lock(m_lastLineMutex);
    return m_lastLine;
}

void ProcessLauncher::ReaderThreadFunc(HANDLE pipeRead) {
    char buffer[4096];
    std::string lineBuffer;

    while (m_running || IsRunning()) {
        DWORD bytesRead = 0;
        DWORD bytesAvail = 0;

        // Check if data available (non-blocking check)
        if (!PeekNamedPipe(pipeRead, nullptr, 0, nullptr, &bytesAvail, nullptr)) {
            break; // Pipe broken
        }

        if (bytesAvail == 0) {
            if (!IsRunning()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // Read available data
        DWORD toRead = (std::min)(bytesAvail, static_cast<DWORD>(sizeof(buffer) - 1));
        if (!ReadFile(pipeRead, buffer, toRead, &bytesRead, nullptr) || bytesRead == 0) {
            break;
        }

        buffer[bytesRead] = '\0';
        lineBuffer += buffer;

        // Process complete lines
        size_t pos;
        while ((pos = lineBuffer.find('\n')) != std::string::npos) {
            std::string line = lineBuffer.substr(0, pos);
            // Remove carriage return
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (!line.empty()) {
                // Classify log level based on content
                spdlog::level::level_enum level = spdlog::level::info;
                std::string lowerLine = line;
                std::transform(lowerLine.begin(), lowerLine.end(), lowerLine.begin(), ::tolower);
                if (lowerLine.find("error") != std::string::npos ||
                    lowerLine.find("fatal") != std::string::npos) {
                    level = spdlog::level::err;
                } else if (lowerLine.find("warn") != std::string::npos) {
                    level = spdlog::level::warn;
                }

                m_log.Push(LogEntry{line, level, m_name, std::chrono::system_clock::now()});

                std::lock_guard<std::mutex> lock(m_lastLineMutex);
                m_lastLine = line;
            }

            lineBuffer.erase(0, pos + 1);
        }
    }

    // Flush remaining
    if (!lineBuffer.empty()) {
        m_log.Push(LogEntry{lineBuffer, spdlog::level::info, m_name,
                            std::chrono::system_clock::now()});
    }

    CloseHandle(pipeRead);
    m_running = false;

    m_log.Push(m_name + " process exited", spdlog::level::info);
}
