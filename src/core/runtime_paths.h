#pragma once

#include <filesystem>

struct RuntimePaths {
    std::filesystem::path settings;
    std::filesystem::path items;
    std::filesystem::path log;
};

std::filesystem::path GetExecutablePath();
RuntimePaths ResolveRuntimePaths(const std::filesystem::path& executable,
                                 const std::filesystem::path& configOverride = {});
