#include "core/config.h"
#include "core/pattern_scanner.h"
#include "core/process.h"
#include "core/runtime_paths.h"
#include "sdk/gnames.h"
#include "sdk/gworld.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#include <spdlog/spdlog.h>

#define CHECK(condition) do { \
    if (!(condition)) throw std::runtime_error( \
        std::string(#condition) + " at line " + std::to_string(__LINE__)); \
} while (false)

namespace fs = std::filesystem;

namespace {
struct TempDirectory {
    fs::path original = fs::current_path();
    fs::path path;
    TempDirectory() {
        std::string pattern = (fs::temp_directory_path() / "dad-startup-XXXXXX").string();
        char* result = mkdtemp(pattern.data());
        if (!result) throw std::runtime_error("mkdtemp failed");
        path = result;
    }
    ~TempDirectory() {
        fs::current_path(original);
        fs::remove_all(path);
    }
};

void WriteFile(const fs::path& file, const std::string& contents) {
    fs::create_directories(file.parent_path());
    std::ofstream output(file);
    output << contents;
    CHECK(output.good());
}

void TestPathsAndConfig() {
    TempDirectory temp;
    const fs::path project = temp.path / "project with spaces";
    const fs::path executable = project / "build-test/tool";
    const fs::path settings = project / "config/settings.json";
    const fs::path unrelated = temp.path / "unrelated";
    const std::string working = R"({"offsets":{"gworld":"0xE06F918","gnames":"0xDF1D980","gobjects":"0xDFE9600"}})";
    WriteFile(executable, "");
    WriteFile(settings, working);
    WriteFile(unrelated / "config/settings.json", R"({"offsets":{"gworld":"0xC36F138"}})");
    fs::current_path(unrelated);

    const RuntimePaths paths = ResolveRuntimePaths(executable);
    CHECK(paths.settings == settings);
    CHECK(paths.items == project / "config/items.json");
    CHECK(paths.log == project / "dad_research.log");
    Config config(paths.settings.string());
    CHECK(config.IsLoaded());
    CHECK(config.Get().gworldOffset == 0xE06F918);
    CHECK(config.Get().gnamesOffset == 0xDF1D980);
    CHECK(config.Get().gobjectsOffset == 0xDFE9600);
    std::cout << "PASS: unrelated cwd cannot select stale config\n";

    fs::create_symlink(executable, unrelated / "tool-link");
    CHECK(ResolveRuntimePaths(unrelated / "tool-link").settings == settings);
    WriteFile(project / "build/Release/tool", "");
    CHECK(ResolveRuntimePaths(project / "build/Release/tool").settings == settings);
    WriteFile(project / "build-test/config/settings.json", working);
    CHECK(ResolveRuntimePaths(executable).settings == project / "build-test/config/settings.json");
    std::cout << "PASS: symlink, nested build and packaged layouts\n";

    WriteFile(unrelated / "custom.json", working);
    const auto custom = ResolveRuntimePaths(executable, "custom.json");
    CHECK(custom.settings == unrelated / "custom.json");
    CHECK(custom.items == unrelated / "items.json");
    const auto missing = ResolveRuntimePaths(executable, "missing.json");
    Config absent(missing.settings.string());
    CHECK(!absent.IsLoaded());
    CHECK(!fs::exists(missing.settings));
    CHECK(absent.Get().gworldOffset == 0 && absent.Get().gnamesOffset == 0);
    CHECK(absent.Get().gobjectsOffset == 0);
    std::cout << "PASS: explicit config is authoritative; missing files do not activate legacy offsets\n";

    fs::current_path(project);
    Config relative("config/settings.json");
    fs::current_path(unrelated);
    relative.Get().filter.maxDistance = 123.0f;
    relative.Save();
    Config saved(settings.string());
    CHECK(saved.IsLoaded() && saved.Get().filter.maxDistance == 123.0f);
    Config other((unrelated / "config/settings.json").string());
    CHECK(other.Get().gworldOffset == 0xC36F138);
    std::cout << "PASS: settings save stays anchored after cwd changes\n";

    WriteFile(settings, R"({"offsets":{"gworld":"0xC36F138","gnames":"0xDF1D980junk"}})");
    CHECK(!config.Load());
    CHECK(!config.IsLoaded());
    CHECK(config.Get().gworldOffset == 0xE06F918);
    WriteFile(settings, "{invalid json");
    CHECK(!config.Load());
    CHECK(config.Get().gnamesOffset == 0xDF1D980);
    WriteFile(settings, working);
    CHECK(config.Load() && config.IsLoaded());
    std::cout << "PASS: invalid config is rejected without partial offset updates\n";
}

template<size_t N, typename T>
void Store(std::array<uint8_t, N>& buffer, size_t offset, const T& value) {
    CHECK(offset + sizeof(value) <= buffer.size());
    std::memcpy(buffer.data() + offset, &value, sizeof(value));
}

void TestReaders() {
    // Exercise only this test process's memory; no game or elevation is needed.
    Process process;
    CHECK(process.Attach(static_cast<DWORD>(getpid())));

    alignas(8) std::array<uint8_t, 0x300> worldData{};
    alignas(8) std::array<uint8_t, 0x60> gameInstance{};
    alignas(8) std::array<uint8_t, 0x40> level{};
    alignas(8) std::array<uintptr_t, 3> objectHeader{};
    const uintptr_t marker = reinterpret_cast<uintptr_t>(objectHeader.data());
    Store(worldData, 0, marker);
    Store(worldData, UEOffsets::UObject_Class, marker);
    Store(worldData, UEOffsets::UWorld_PersistentLevel, reinterpret_cast<uintptr_t>(level.data()));
    Store(worldData, UEOffsets::UWorld_OwningGameInstance, reinterpret_cast<uintptr_t>(gameInstance.data()));
    uintptr_t globalWorld = reinterpret_cast<uintptr_t>(worldData.data());
    const uintptr_t globalAddress = reinterpret_cast<uintptr_t>(&globalWorld);
    GWorldReader world;
    CHECK(world.Initialize(process, globalAddress));
    CHECK(world.GetUWorld(process) == globalWorld);

    globalWorld = 0x69007400630065;
    CHECK(world.GetUWorld(process) == 0);
    CHECK(world.GetLocalPlayerController(process) == 0);
    CHECK(world.GetAllActors(process).empty());
    CHECK(!world.Initialize(process, globalAddress));
    CHECK(world.GetAddress() == 0);
    std::cout << "PASS: text bytes are rejected as UWorld at startup and runtime\n";

    globalWorld = reinterpret_cast<uintptr_t>(worldData.data());
    Store(worldData, UEOffsets::UObject_Class, uintptr_t{0});
    CHECK(!world.Initialize(process, globalAddress));
    Store(worldData, UEOffsets::UObject_Class, marker);
    Store(worldData, UEOffsets::UWorld_OwningGameInstance, uintptr_t{0});
    CHECK(!world.Initialize(process, globalAddress));
    Store(worldData, UEOffsets::UWorld_OwningGameInstance, reinterpret_cast<uintptr_t>(gameInstance.data()));
    CHECK(world.Initialize(process, globalAddress));
    TArray<uintptr_t> invalidPlayers{marker, 200000, 200000};
    Store(gameInstance, UEOffsets::GameInstance_LocalPlayers, invalidPlayers);
    CHECK(world.GetLocalPlayerController(process) == 0);
    std::cout << "PASS: malformed world and local-player chains fail validation\n";

    alignas(8) std::array<uint8_t, 128> pool{};
    alignas(8) std::array<uint8_t, 16> entry{};
    Store(entry, 0, uint16_t{4 << 6});
    std::memcpy(entry.data() + 2, "None", 4);
    Store(pool, 0x10, reinterpret_cast<uintptr_t>(entry.data()));
    GNamesReader names;
    CHECK(names.Initialize(process, reinterpret_cast<uintptr_t>(pool.data())));
    CHECK(names.GetName(process, 0) == "None");
    std::memcpy(entry.data() + 2, "Nope", 4);
    CHECK(!names.Initialize(process, reinterpret_cast<uintptr_t>(pool.data())));
    CHECK(names.GetName(process, 0).empty());
    std::cout << "PASS: invalid name pool cannot reuse a previously verified cache\n";

    CHECK(PatternScanner::ResolveRelative(process, 1, 3, 7) == 0);
    std::cout << "PASS: failed relative-address reads cannot become offset candidates\n";
}

void TestManualProxyBase() {
    Process process;
    CHECK(process.Attach(getpid()));

    // 1. Invalid manual base marks failure
    process.SetManualProxyBase(0x12345678);
    CHECK(process.GetManualProxyBase() == 0x12345678);
    CHECK(process.ManualProxyBaseFailed());
    CHECK(!process.IsProxyConnected());
    std::cout << "PASS: invalid manual base reports failure\n";

    // 2. Setting base 0 resets to auto-scan and clears failure
    process.SetManualProxyBase(0);
    CHECK(process.GetManualProxyBase() == 0);
    CHECK(!process.ManualProxyBaseFailed());
    std::cout << "PASS: resetting manual base to 0 restores auto-scan state\n";

    // 3. Direct SharedGameData resolution
    alignas(16) SharedGameData directShm = {};
    directShm.magic = EXT_SHM_MAGIC;
    directShm.version = EXT_SHM_VERSION;
    directShm.pid = getpid();
    directShm.heartbeat = 42;
    directShm.gameBase = 0x140000000;
    directShm.gameSize = 0x5000000;
    directShm.proxyBase = reinterpret_cast<uint64_t>(&directShm);

    process.SetManualProxyBase(reinterpret_cast<uintptr_t>(&directShm));
    CHECK(process.IsProxyConnected());
    CHECK(!process.ManualProxyBaseFailed());
    CHECK(process.GetSharedData() != nullptr);
    CHECK(process.GetSharedData()->heartbeat == 42);
    CHECK(process.GetSharedData()->gameBase == 0x140000000);
    CHECK(process.GetProxyHeartbeat() == 42);
    std::cout << "PASS: direct SharedGameData address resolution\n";

    // 4. Manually mapped module simulation (scanning within module region)
    std::vector<uint8_t> dummyDll(0x20000, 0); // 128 KB simulated DLL image
    auto* dllShm = reinterpret_cast<SharedGameData*>(dummyDll.data() + 0x8000);
    dllShm->magic = EXT_SHM_MAGIC;
    dllShm->version = EXT_SHM_VERSION;
    dllShm->pid = getpid();
    dllShm->heartbeat = 99;
    dllShm->gameBase = 0x7FF70000;
    dllShm->proxyBase = reinterpret_cast<uint64_t>(dummyDll.data());

    process.SetManualProxyBase(reinterpret_cast<uintptr_t>(dummyDll.data()));
    CHECK(process.IsProxyConnected());
    CHECK(!process.ManualProxyBaseFailed());
    CHECK(process.GetSharedData() != nullptr);
    CHECK(process.GetSharedData()->heartbeat == 99);
    CHECK(process.GetRemoteShmAddress() == reinterpret_cast<uintptr_t>(dllShm));
    CHECK(process.GetProxyBase() == reinterpret_cast<uintptr_t>(dummyDll.data()));
    std::cout << "PASS: manually mapped module image base resolution\n";

    // 5. IPC target synchronization
    SilentHelperIPC ipc = {};
    ipc.targetX = 123.4;
    ipc.hasTarget = 1;
    process.SyncProxyTarget(ipc);
    CHECK(dllShm->silentHelper.targetX == 123.4);
    CHECK(dllShm->silentHelper.hasTarget == 1);
    std::cout << "PASS: proxy target synchronization via resolved manual base\n";
}
} // namespace

int main() {
    spdlog::set_level(spdlog::level::off);
    try {
        TestPathsAndConfig();
        TestReaders();
        TestManualProxyBase();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
