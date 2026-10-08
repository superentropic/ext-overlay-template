#pragma once
// ============================================================================
//  Runtime Integrity Verification
//
//  Verifies that critical source files have not been tampered with to add
//  cheat capabilities (memory writes, input injection, aimbot, etc.).
//
//  This is a DETERRENT, not bulletproof DRM. The goal is to make it obvious
//  when someone forks the repo and adds malicious code, and to make automated
//  "cheat builder" pipelines fail without manual intervention.
//
//  How it works:
//  1. At build time, generate_hashes.py computes SHA-256 of critical files
//  2. Hashes are embedded in integrity_hashes.h (checked into git)
//  3. At startup, if source files are present alongside the binary,
//     we re-hash them and compare. Mismatch = tampered.
//  4. The binary also embeds a compile-time signature that can be verified.
// ============================================================================

#include <string>
#include <vector>

namespace Integrity {

struct CheckResult {
    bool passed = true;
    std::vector<std::string> warnings;   // Non-fatal issues
    std::vector<std::string> failures;   // Tampered files
};

// Run integrity checks. Returns result with details.
// If source files aren't present (normal for end users), check passes.
// If source files ARE present and hashes mismatch, check fails.
CheckResult Verify();

// Get a fingerprint string for the build (embedded at compile time)
const char* GetBuildFingerprint();

} // namespace Integrity
