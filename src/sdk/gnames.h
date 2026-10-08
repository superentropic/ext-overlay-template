#pragma once
#include "core/process.h"
#include <string>
#include <unordered_map>
#include <cstdint>

class GNamesReader {
public:
    bool Initialize(const Process& proc, uintptr_t gNamesAddr);

    // Resolve FName index to string
    std::string GetName(const Process& proc, int32_t index);

    // Resolve FName to full string (with number suffix if applicable)
    std::string GetFName(const Process& proc, int32_t comparisonIndex, int32_t number);

    uintptr_t GetAddress() const { return m_poolAddress; }

    // Clear the name cache (useful after game level changes)
    void ClearCache() { m_cache.clear(); }

private:
    uintptr_t m_poolAddress = 0;

    // UE5 FNamePool uses chunked allocation
    // Block size depends on engine version — typically 0x2000 or 0x4000
    static constexpr int BLOCK_SIZE = 0x2000;
    static constexpr int BLOCK_OFFSET_BITS = 16;

    // Cache for resolved names to avoid repeated RPM calls
    std::unordered_map<int32_t, std::string> m_cache;

    // Read the raw name entry from the pool
    std::string ReadNameEntry(const Process& proc, uintptr_t entryAddr);
};
