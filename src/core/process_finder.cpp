#include "process_finder.h"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#include <TlHelp32.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

namespace {

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}

std::wstring Basename(std::wstring value) {
    const auto slash = value.find_last_of(L"/\\");
    if (slash != std::wstring::npos)
        value.erase(0, slash + 1);
    return value;
}

bool NamesMatch(const std::wstring& actual, const std::wstring& requested) {
    const std::wstring actualName = Lower(Basename(actual));
    const std::wstring requestedName = Lower(Basename(requested));
    if (actualName == requestedName)
        return true;

#ifndef _WIN32
    // Windows game names are commonly supplied with .exe even when the
    // corresponding Linux process is launched without the suffix.
    constexpr wchar_t exeSuffix[] = L".exe";
    if (requestedName.size() > 4 &&
        requestedName.compare(requestedName.size() - 4, 4, exeSuffix) == 0 &&
        actualName == requestedName.substr(0, requestedName.size() - 4)) {
        return true;
    }
    if (actualName.size() > 4 &&
        actualName.compare(actualName.size() - 4, 4, exeSuffix) == 0 &&
        requestedName == actualName.substr(0, actualName.size() - 4)) {
        return true;
    }
#endif
    return false;
}

#ifndef _WIN32
std::wstring ReadProcText(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file)
        return {};

    std::string value;
    std::getline(file, value, '\0');
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
        value.pop_back();
    return std::wstring(value.begin(), value.end());
}

bool IsNumeric(const std::string& value) {
    return !value.empty() &&
        std::all_of(value.begin(), value.end(),
            [](unsigned char c) { return std::isdigit(c) != 0; });
}
#endif

} // namespace

uint32_t FindProcessIdByName(const std::wstring& processName) {
#ifdef _WIN32
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    uint32_t pid = 0;

    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (NamesMatch(entry.szExeFile, processName)) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return pid;
#else
    const std::filesystem::path procRoot("/proc");
    std::error_code error;

    for (const auto& entry : std::filesystem::directory_iterator(procRoot, error)) {
        if (error)
            break;

        const std::string pidText = entry.path().filename().string();
        const bool isDirectory = entry.is_directory(error);
        if (error) {
            error.clear();
            continue;
        }
        if (!isDirectory || !IsNumeric(pidText))
            continue;

        const uint32_t pid = static_cast<uint32_t>(std::stoul(pidText));
        std::vector<std::wstring> candidates;

        // /proc/<pid>/comm is cheap and works even when /proc/<pid>/exe is
        // inaccessible because of ptrace restrictions.
        const std::wstring comm = ReadProcText(entry.path() / "comm");
        if (!comm.empty())
            candidates.push_back(comm);

        std::error_code linkError;
        const auto executable = std::filesystem::read_symlink(
            entry.path() / "exe", linkError);
        if (!linkError)
            candidates.push_back(executable.filename().wstring());

        // Wine/Proton commonly puts wine-preloader in argv[0] and the game
        // executable in a later argv entry, so inspect every NUL-separated
        // argument rather than only argv[0].
        std::ifstream cmdlineFile(entry.path() / "cmdline", std::ios::binary);
        if (cmdlineFile) {
            const std::string raw((std::istreambuf_iterator<char>(cmdlineFile)),
                                  std::istreambuf_iterator<char>());
            size_t begin = 0;
            while (begin < raw.size()) {
                const size_t end = raw.find('\0', begin);
                const size_t length = end == std::string::npos ? raw.size() - begin : end - begin;
                if (length)
                    candidates.emplace_back(raw.begin() + begin, raw.begin() + begin + length);
                if (end == std::string::npos)
                    break;
                begin = end + 1;
            }
        }

        if (std::any_of(candidates.begin(), candidates.end(),
                [&](const std::wstring& candidate) {
                    return NamesMatch(candidate, processName);
                })) {
            return pid;
        }

        // A Proton process can hide the game name from comm/exe/cmdline but
        // still map the PE image. Search mapped file names as a final fallback.
        std::ifstream maps(entry.path() / "maps");
        std::string mapLine;
        while (std::getline(maps, mapLine)) {
            const auto pathStart = mapLine.find('/');
            if (pathStart == std::string::npos)
                continue;
            const std::wstring mappedPath(mapLine.begin() + pathStart, mapLine.end());
            if (NamesMatch(mappedPath, processName))
                return pid;
        }
    }

    return 0;
#endif
}
