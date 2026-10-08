#include "runtime_paths.h"

#include <stdexcept>
#ifdef _WIN32
#include <Windows.h>
#include <vector>
#endif

std::filesystem::path GetExecutablePath() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                           static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size())
        throw std::runtime_error("Cannot resolve the executable path");
    return std::filesystem::path(std::wstring(buffer.data(), length));
#else
    return std::filesystem::canonical("/proc/self/exe");
#endif
}

RuntimePaths ResolveRuntimePaths(const std::filesystem::path& executable,
                                 const std::filesystem::path& configOverride) {
    namespace fs = std::filesystem;
    fs::path settings;
    if (!configOverride.empty()) {
        // Only an explicit override is interpreted relative to the caller's cwd.
        settings = fs::absolute(configOverride).lexically_normal();
    } else {
        const fs::path binaryDir = fs::weakly_canonical(executable).parent_path();
        settings = binaryDir / "config/settings.json";
        fs::path directory = binaryDir;
        // Packaged binary, Linux build directory, or Windows build/Release.
        for (int depth = 0; depth < 3; ++depth) {
            const fs::path candidate = directory / "config/settings.json";
            if (fs::is_regular_file(candidate)) {
                settings = candidate;
                break;
            }
            if (directory == directory.parent_path()) break;
            directory = directory.parent_path();
        }
    }

    const fs::path configDir = settings.parent_path();
    const fs::path logDir = configDir.filename() == "config"
        ? configDir.parent_path() : configDir;
#ifdef _WIN32
    const fs::path logFile = logDir / "ext_research.log";
#else
    /* On Linux keep the log in /tmp/opencode (tmpfs — no on-disk trace) */
    const fs::path logFile = "/tmp/opencode/er.log";
#endif
    return {settings, configDir / "items.json", logFile};
}
