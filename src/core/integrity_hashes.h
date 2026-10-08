#pragma once
// ============================================================================
//  AUTO-GENERATED -- Do not edit manually!
//  Run: python tools/generate_hashes.py --generate
//
//  SHA-256 hashes of critical source files. These files define the tool's
//  read-only research behavior. If any hash mismatches at build time,
//  it means a critical file was modified -- review the change carefully
//  to ensure no write/inject/aim capabilities were added.
// ============================================================================

#include <array>
#include <string_view>
#include <utility>

namespace Integrity {

static constexpr size_t FILE_COUNT = 10;

// {filename, expected_sha256}
static constexpr std::array<std::pair<std::string_view, std::string_view>, FILE_COUNT> EXPECTED_HASHES = {{
    {"src/core/process.cpp", "d9ff937defcc702c0b75e87a7476e104512a6ae7fc35f1e1defecfbbf4fa285b"},
    {"src/core/process.h", "3f825faa0b6cd5eae1e41fa8fb49d439310bcf60305d5afd7eb16627df7083ab"},
    {"src/game/entity.h", "f96e728613f4d87dd987f209778b1f67b75f3c14210a7919bd5ae9679c7ec81a"},
    {"src/main.cpp", "3a27bc0b930b8b48b5133c31237349f1d10bc1416cdd39466b61eccdd8ff2dd3"},
    {"src/render/drawing.cpp", "bd9f63412afad2ef5fc7e9df2b21a59c09d6022dc74edf3de534289497ee3812"},
    {"src/render/drawing.h", "b70498891f228dc70e50309771578cdea4640b983b83ad4785c08fdee4d81905"},
    {"src/render/overlay.cpp", "9720d2d1126a9f232f85631ea187a3bcf2639f95db2c86e00944788fa7d2a057"},
    {"src/render/overlay.h", "98f70c145f15b1af0a074683480b4565c8559a52f1a3f9ca60503a7a54cf97a6"},
    {"src/render/world_to_screen.cpp", "31d023e025bd7741e2d9d9a01e1d04496157ed75b5fc6cbceb1e1525b1633826"},
    {"src/render/world_to_screen.h", "1554cd520b51176a501a5148fc119e8661df20f02624270a9e7c070cbfc22582"},
}};

} // namespace Integrity
