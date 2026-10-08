#include "integrity.h"
#include "integrity_hashes.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <array>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <iomanip>

// Platform-independent SHA-256 implementation (no OpenSSL dependency)
// Based on RFC 6234 / FIPS 180-4
namespace {

class SHA256 {
public:
    SHA256() { Reset(); }

    void Update(const uint8_t* data, size_t len) {
        for (size_t i = 0; i < len; i++) {
            m_data[m_blockLen++] = data[i];
            if (m_blockLen == 64) {
                Transform();
                m_bitLen += 512;
                m_blockLen = 0;
            }
        }
    }

    std::string Final() {
        uint32_t i = m_blockLen;

        // Padding
        if (m_blockLen < 56) {
            m_data[i++] = 0x80;
            while (i < 56) m_data[i++] = 0x00;
        } else {
            m_data[i++] = 0x80;
            while (i < 64) m_data[i++] = 0x00;
            Transform();
            memset(m_data, 0, 56);
        }

        m_bitLen += m_blockLen * 8;
        m_data[63] = static_cast<uint8_t>(m_bitLen);
        m_data[62] = static_cast<uint8_t>(m_bitLen >> 8);
        m_data[61] = static_cast<uint8_t>(m_bitLen >> 16);
        m_data[60] = static_cast<uint8_t>(m_bitLen >> 24);
        m_data[59] = static_cast<uint8_t>(m_bitLen >> 32);
        m_data[58] = static_cast<uint8_t>(m_bitLen >> 40);
        m_data[57] = static_cast<uint8_t>(m_bitLen >> 48);
        m_data[56] = static_cast<uint8_t>(m_bitLen >> 56);
        Transform();

        // Format as hex string
        std::ostringstream oss;
        for (int j = 0; j < 8; j++) {
            oss << std::hex << std::setfill('0') << std::setw(8) << m_state[j];
        }
        return oss.str();
    }

private:
    uint32_t m_state[8]{};
    uint8_t m_data[64]{};
    uint32_t m_blockLen = 0;
    uint64_t m_bitLen = 0;

    static constexpr uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
        0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
        0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    };

    static uint32_t RotR(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    static uint32_t Ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
    static uint32_t Maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
    static uint32_t Sig0(uint32_t x) { return RotR(x, 2) ^ RotR(x, 13) ^ RotR(x, 22); }
    static uint32_t Sig1(uint32_t x) { return RotR(x, 6) ^ RotR(x, 11) ^ RotR(x, 25); }
    static uint32_t sig0(uint32_t x) { return RotR(x, 7) ^ RotR(x, 18) ^ (x >> 3); }
    static uint32_t sig1(uint32_t x) { return RotR(x, 17) ^ RotR(x, 19) ^ (x >> 10); }

    void Reset() {
        m_state[0] = 0x6a09e667; m_state[1] = 0xbb67ae85;
        m_state[2] = 0x3c6ef372; m_state[3] = 0xa54ff53a;
        m_state[4] = 0x510e527f; m_state[5] = 0x9b05688c;
        m_state[6] = 0x1f83d9ab; m_state[7] = 0x5be0cd19;
        m_blockLen = 0;
        m_bitLen = 0;
    }

    void Transform() {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = (m_data[i * 4] << 24) | (m_data[i * 4 + 1] << 16) |
                   (m_data[i * 4 + 2] << 8) | m_data[i * 4 + 3];
        }
        for (int i = 16; i < 64; i++) {
            w[i] = sig1(w[i - 2]) + w[i - 7] + sig0(w[i - 15]) + w[i - 16];
        }

        uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
        uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];

        for (int i = 0; i < 64; i++) {
            uint32_t t1 = h + Sig1(e) + Ch(e, f, g) + K[i] + w[i];
            uint32_t t2 = Sig0(a) + Maj(a, b, c);
            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }

        m_state[0] += a; m_state[1] += b; m_state[2] += c; m_state[3] += d;
        m_state[4] += e; m_state[5] += f; m_state[6] += g; m_state[7] += h;
    }
};

std::string HashFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return "";

    // Read entire file
    std::string content((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());

    // Normalize CRLF -> LF (same as generate_hashes.py)
    std::string normalized;
    normalized.reserve(content.size());
    for (size_t i = 0; i < content.size(); i++) {
        if (content[i] == '\r' && i + 1 < content.size() && content[i + 1] == '\n') {
            continue; // Skip \r in \r\n
        }
        normalized += content[i];
    }

    SHA256 sha;
    sha.Update(reinterpret_cast<const uint8_t*>(normalized.data()), normalized.size());
    return sha.Final();
}

// Try to find the source root relative to the executable
std::filesystem::path FindSourceRoot() {
    // The exe is typically in build/Release/ — source root is ../../
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    auto exeDir = std::filesystem::path(exePath).parent_path();

    // Try common layouts: build/Release/exe -> ../../src
    std::array<std::filesystem::path, 3> candidates = {
        exeDir / ".." / ".." / "src",       // build/Release/
        exeDir / ".." / "src",              // build/
        exeDir / "src",                     // Running from repo root
    };

    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate / "main.cpp")) {
            return candidate.parent_path();
        }
    }

    return {}; // Source not found (normal for distributed binary)
}

} // anonymous namespace

namespace Integrity {

// Build fingerprint — embedded at compile time
static constexpr const char* BUILD_FINGERPRINT =
    "DAD-RT|" __DATE__ "|" __TIME__ "|"
#ifdef NDEBUG
    "Release"
#else
    "Debug"
#endif
    ;

CheckResult Verify() {
    CheckResult result;

    auto sourceRoot = FindSourceRoot();
    if (sourceRoot.empty()) {
        // Source files not present — this is a distributed binary.
        // Can't verify source integrity, but that's expected.
        result.warnings.push_back("Source files not found (distributed binary) - skipping source integrity check");
        return result;
    }

    int verified = 0;
    for (const auto& [filename, expectedHash] : EXPECTED_HASHES) {
        auto filePath = sourceRoot / std::filesystem::path(std::string(filename));

        if (!std::filesystem::exists(filePath)) {
            result.warnings.push_back("Missing: " + std::string(filename));
            continue;
        }

        std::string actualHash = HashFile(filePath);
        if (actualHash.empty()) {
            result.warnings.push_back("Could not read: " + std::string(filename));
            continue;
        }

        if (actualHash != expectedHash) {
            result.passed = false;
            result.failures.push_back(
                "TAMPERED: " + std::string(filename) +
                " (expected " + std::string(expectedHash).substr(0, 16) + "..." +
                ", got " + actualHash.substr(0, 16) + "...)");
        } else {
            verified++;
        }
    }

    if (verified == static_cast<int>(FILE_COUNT)) {
        // All files verified successfully
    } else if (result.passed && verified > 0) {
        result.warnings.push_back(
            "Verified " + std::to_string(verified) + "/" +
            std::to_string(FILE_COUNT) + " critical files");
    }

    return result;
}

const char* GetBuildFingerprint() {
    return BUILD_FINGERPRINT;
}

} // namespace Integrity
