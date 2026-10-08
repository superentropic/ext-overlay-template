#include "pattern_scanner.h"
#include <spdlog/spdlog.h>
#include <sstream>

std::vector<PatternScanner::PatternByte> PatternScanner::ParsePattern(const std::string& pattern) {
    std::vector<PatternByte> bytes;
    std::istringstream stream(pattern);
    std::string token;

    while (stream >> token) {
        if (token == "?" || token == "??") {
            bytes.push_back({0, true});
        } else {
            bytes.push_back({static_cast<uint8_t>(std::stoul(token, nullptr, 16)), false});
        }
    }
    return bytes;
}

std::optional<uintptr_t> PatternScanner::FindPattern(
    const Process& proc,
    const std::wstring& moduleName,
    const std::string& pattern) const
{
    uintptr_t moduleBase = proc.GetModuleBase(moduleName);
    DWORD moduleSize = proc.GetModuleSize(moduleName);

    if (!moduleBase || !moduleSize) {
        spdlog::error("Module not found for pattern scan");
        return std::nullopt;
    }

    auto patternBytes = ParsePattern(pattern);
    if (patternBytes.empty()) {
        spdlog::error("Empty pattern");
        return std::nullopt;
    }

    // Read module memory in chunks to reduce RPM calls
    constexpr size_t CHUNK_SIZE = 0x10000; // 64KB chunks
    std::vector<uint8_t> buffer(CHUNK_SIZE);

    for (size_t offset = 0; offset < moduleSize; offset += CHUNK_SIZE) {
        size_t readSize = (std::min)(static_cast<size_t>(CHUNK_SIZE),
                                      static_cast<size_t>(moduleSize - offset));

        if (!proc.ReadRaw(moduleBase + offset, buffer.data(), readSize))
            continue;
        if (readSize < patternBytes.size())
            continue;

        for (size_t i = 0; i <= readSize - patternBytes.size(); ++i) {
            bool found = true;
            for (size_t j = 0; j < patternBytes.size(); ++j) {
                if (!patternBytes[j].wildcard && buffer[i + j] != patternBytes[j].value) {
                    found = false;
                    break;
                }
            }
            if (found) {
                uintptr_t result = moduleBase + offset + i;
                spdlog::info("Pattern found at 0x{:X}", result);
                return result;
            }
        }
    }

    spdlog::warn("Pattern not found: {}", pattern);
    return std::nullopt;
}

uintptr_t PatternScanner::ResolveRelative(
    const Process& proc,
    uintptr_t addr,
    int ripOffset,
    int instrSize)
{
    int32_t relativeOffset = 0;
    if (!addr || ripOffset < 0 || instrSize <= 0 ||
        !proc.ReadRaw(addr + ripOffset, &relativeOffset, sizeof(relativeOffset)))
        return 0;
    return addr + instrSize + relativeOffset;
}
