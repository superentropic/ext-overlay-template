#pragma once
#include "process.h"
#include <string>
#include <vector>
#include <optional>

class PatternScanner {
public:
    // Scan within a module's memory range
    // Pattern format: "48 8B 05 ? ? ? ? 48 8B 88" where ? is wildcard
    std::optional<uintptr_t> FindPattern(
        const Process& proc,
        const std::wstring& moduleName,
        const std::string& pattern) const;

    // Resolve RIP-relative address:
    // instruction at addr, relative offset at addr+ripOffset, instruction length = instrSize
    // result = addr + instrSize + *(int32_t*)(addr + ripOffset)
    static uintptr_t ResolveRelative(
        const Process& proc,
        uintptr_t addr,
        int ripOffset,
        int instrSize);

private:
    struct PatternByte {
        uint8_t value;
        bool wildcard;
    };

    static std::vector<PatternByte> ParsePattern(const std::string& pattern);
};
