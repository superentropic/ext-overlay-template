#include "gnames.h"
#include <cstring>
#include <spdlog/spdlog.h>

bool GNamesReader::Initialize(const Process& proc, uintptr_t gNamesAddr) {
    m_poolAddress = gNamesAddr;
    m_cache.clear();

    if (!m_poolAddress) {
        spdlog::error("GNames address is null");
        return false;
    }

    // Diagnostic: dump the first 128 bytes at the GNames address to understand layout
    uint8_t headerDump[128] = {};
    if (proc.ReadRaw(m_poolAddress, headerDump, sizeof(headerDump))) {
        spdlog::info("GNames hex dump at 0x{:X}:", m_poolAddress);
        for (int row = 0; row < 8; row++) {
            int off = row * 16;
            spdlog::info("  +0x{:02X}: {:02X} {:02X} {:02X} {:02X}  {:02X} {:02X} {:02X} {:02X}  "
                         "{:02X} {:02X} {:02X} {:02X}  {:02X} {:02X} {:02X} {:02X}",
                off,
                headerDump[off+0], headerDump[off+1], headerDump[off+2], headerDump[off+3],
                headerDump[off+4], headerDump[off+5], headerDump[off+6], headerDump[off+7],
                headerDump[off+8], headerDump[off+9], headerDump[off+10], headerDump[off+11],
                headerDump[off+12], headerDump[off+13], headerDump[off+14], headerDump[off+15]);
        }

        // Try to interpret as pointers at various offsets
        for (int off = 0; off <= 0x40; off += 8) {
            uintptr_t val = 0;
            std::memcpy(&val, headerDump + off, sizeof(val));
            // Check if it looks like a pointer (in valid range)
            if (val > 0x10000 && val < 0x00007FFFFFFFFFFF) {
                spdlog::info("  Potential pointer at +0x{:02X}: 0x{:X}", off, val);

                // Try to read at that pointer to see if it's a block pointer
                uint8_t testBlock[16] = {};
                if (proc.ReadRaw(val, testBlock, sizeof(testBlock))) {
                    // Check if bytes 2-5 at the target look like "None" (ASCII)
                    // FNameEntry for "None": header(2 bytes) + "None"(4 bytes)
                    spdlog::info("    -> data: {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}  "
                                 "'{}{}{}{}{}{}'",
                        testBlock[0], testBlock[1], testBlock[2], testBlock[3],
                        testBlock[4], testBlock[5], testBlock[6], testBlock[7],
                        (testBlock[2] >= 32 && testBlock[2] < 127) ? (char)testBlock[2] : '.',
                        (testBlock[3] >= 32 && testBlock[3] < 127) ? (char)testBlock[3] : '.',
                        (testBlock[4] >= 32 && testBlock[4] < 127) ? (char)testBlock[4] : '.',
                        (testBlock[5] >= 32 && testBlock[5] < 127) ? (char)testBlock[5] : '.',
                        (testBlock[6] >= 32 && testBlock[6] < 127) ? (char)testBlock[6] : '.',
                        (testBlock[7] >= 32 && testBlock[7] < 127) ? (char)testBlock[7] : '.');
                }
            }
        }
    } else {
        spdlog::error("Failed to read GNames header at 0x{:X}", m_poolAddress);
        m_poolAddress = 0;
        return false;
    }

    // Verify by reading a known FName (index 0 = "None")
    std::string testName = GetName(proc, 0);
    if (testName == "None") {
        spdlog::info("GNames initialized at 0x{:X} - verified (index 0 = '{}')", m_poolAddress, testName);
        return true;
    }

    spdlog::warn("GNames at 0x{:X} - index 0 resolved to '{}' (expected 'None')", m_poolAddress, testName);

    // Try treating m_poolAddress as a pointer TO the pool (one more dereference)
    uintptr_t deref = proc.Read<uintptr_t>(m_poolAddress);
    if (deref > 0x10000 && deref < 0x00007FFFFFFFFFFF) {
        spdlog::info("Trying GNames as pointer: *0x{:X} = 0x{:X}", m_poolAddress, deref);
        uintptr_t savedPool = m_poolAddress;
        m_poolAddress = deref;
        m_cache.clear();
        testName = GetName(proc, 0);
        if (testName == "None") {
            spdlog::info("GNames dereferenced! Pool at 0x{:X} - verified (index 0 = '{}')", m_poolAddress, testName);
            return true;
        }
        spdlog::warn("Dereferenced pool at 0x{:X} - index 0 = '{}' (still not 'None')", m_poolAddress, testName);
        m_poolAddress = savedPool;  // revert
        m_cache.clear();
    }

    m_poolAddress = 0;
    m_cache.clear();
    return false;
}

std::string GNamesReader::GetName(const Process& proc, int32_t index) {
    // Check cache first
    auto it = m_cache.find(index);
    if (it != m_cache.end())
        return it->second;

    if (index < 0 || !m_poolAddress)
        return "";

    // UE5 FNamePool layout:
    // The pool is an array of blocks. Each block is an array of FNameEntry pointers.
    //
    // Typical UE5 layout:
    //   FNamePool + 0x00: Lock (4 bytes)
    //   FNamePool + 0x04: CurrentBlock (uint32_t)
    //   FNamePool + 0x08: CurrentByteCursor (uint32_t)
    //   FNamePool + 0x10: Blocks[FNameMaxBlocks] - array of pointers to entry blocks
    //
    // Each entry (FNameEntry):
    //   - Header: 2 bytes (length + flags)
    //   - Data: variable length string (wide or ansi based on header flag)

    int32_t blockIdx = index >> BLOCK_OFFSET_BITS;
    int32_t blockOffset = index & ((1 << BLOCK_OFFSET_BITS) - 1);

    // Read the block pointer
    // Blocks array starts at FNamePool + 0x10 (typical offset)
    constexpr uintptr_t BLOCKS_OFFSET = 0x10;
    uintptr_t blockPtr = proc.Read<uintptr_t>(m_poolAddress + BLOCKS_OFFSET + blockIdx * sizeof(uintptr_t));
    if (!blockPtr)
        return "";

    // Each FNameEntry is accessed by stride
    // In UE5, the block stores entries sequentially with variable sizes
    // The offset within the block is: blockOffset * stride
    // Stride is typically 2 (each unit = 2 bytes of entry data)
    constexpr int ENTRY_STRIDE = 2;
    uintptr_t entryAddr = blockPtr + static_cast<uintptr_t>(blockOffset) * ENTRY_STRIDE;

    std::string name = ReadNameEntry(proc, entryAddr);
    if (!name.empty()) {
        m_cache[index] = name;
    }
    return name;
}

std::string GNamesReader::GetFName(const Process& proc, int32_t comparisonIndex, int32_t number) {
    std::string base = GetName(proc, comparisonIndex);
    if (number > 0) {
        base += "_" + std::to_string(number - 1);
    }
    return base;
}

std::string GNamesReader::ReadNameEntry(const Process& proc, uintptr_t entryAddr) {
    // FNameEntry header (2 bytes):
    //   bit 0: bIsWide (1 = UTF-16, 0 = ANSI)
    //   bits 1-15: length of the string
    uint16_t header = proc.Read<uint16_t>(entryAddr);

    bool isWide = (header & 1) != 0;
    int32_t length = header >> 6; // UE5 uses bits 6-15 for length

    if (length <= 0 || length > 1024)
        return "";

    uintptr_t stringAddr = entryAddr + 2; // String data starts after the 2-byte header

    if (isWide) {
        std::vector<char16_t> wideBuffer(length + 1, 0);
        if (!proc.ReadRaw(stringAddr, wideBuffer.data(), length * 2))
            return "";

        // Convert to narrow string
        std::string result;
        result.reserve(length);
        for (int i = 0; i < length; i++) {
            result += static_cast<char>(wideBuffer[i] & 0xFF);
        }
        return result;
    } else {
        std::string buffer(length, '\0');
        if (!proc.ReadRaw(stringAddr, buffer.data(), length))
            return "";
        return buffer;
    }
}
