#include "config.h"
#include <filesystem>
#include <cstdio>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <spdlog/spdlog.h>

Config::Config(const std::string& filepath)
    : m_filepath(std::filesystem::absolute(filepath).lexically_normal().string()) {
    Load();
}

bool Config::Load() {
    m_loaded = false;
    std::ifstream file(m_filepath);
    if (!file.is_open()) {
        spdlog::error("Cannot open config: {}", m_filepath);
        return false;
    }

    const AppConfig previous = m_config;
    try {
        nlohmann::json j;
        file >> j;
        if (!j.is_object())
            throw std::runtime_error("Config must be a JSON object");

        auto& f = m_config.filter;
        if (j.contains("filter")) {
            auto& jf = j["filter"];
            f.showPlayers = jf.value("showPlayers", true);
            f.showLoot = jf.value("showLoot", true);
            f.showNPCs = jf.value("showNPCs", true);
            f.showPortals = jf.value("showPortals", true);
            f.showCorpses = jf.value("showCorpses", true);
            f.maxDistance = jf.value("maxDistance", 300.0f);
            f.maxPlayerDistance = jf.value("maxPlayerDistance", 1000.0f);
            f.minLootRarity = static_cast<ItemRarity>(jf.value("minLootRarity", 3)); // Default: Uncommon
            f.showHealthBars = jf.value("showHealthBars", true);
            f.showDamageNumbers = jf.value("showDamageNumbers", true);
            f.showDistance = jf.value("showDistance", true);
            f.showSnapLines = jf.value("showSnapLines", false);
            f.showGhostPlayers = jf.value("showGhostPlayers", true);
            f.minMonsterGrade = jf.value("minMonsterGrade", 0);
        }

        if (j.contains("visuals")) {
            auto& jv = j["visuals"];
            if (jv.contains("playerColor")) {
                auto c = jv["playerColor"];
                for (int i = 0; i < 4; i++) m_config.visuals.playerColor[i] = c[i];
            }
            if (jv.contains("teamColor")) {
                auto c = jv["teamColor"];
                for (int i = 0; i < 4; i++) m_config.visuals.teamColor[i] = c[i];
            }
            if (jv.contains("npcColor")) {
                auto c = jv["npcColor"];
                for (int i = 0; i < 4; i++) m_config.visuals.npcColor[i] = c[i];
            }
            m_config.visuals.fontSize = jv.value("fontSize", 15.0f);
            m_config.visuals.boxStyle = jv.value("boxStyle", 0);
            m_config.visuals.radarRadius = jv.value("radarRadius", 140.0f);
            m_config.visuals.radarRangeMeters = jv.value("radarRangeMeters", 100.0f);
            m_config.visuals.minimapRotating = jv.value("minimapRotating", false);
        }

        m_config.updateRateFps = j.value("updateRateFps", 144);
        m_config.toggleMenuKey = j.value("toggleMenuKey", VK_INSERT);
        m_config.externalMode = j.value("externalMode", true);

        if (j.contains("projectileRedirect")) {
            auto& jpr = j["projectileRedirect"];
            m_config.projectileRedirect.enabled = jpr.value("enabled", true);
            m_config.projectileRedirect.customBaseAddress = 0;
            if (jpr.contains("customBaseAddress")) {
                const auto& jb = jpr["customBaseAddress"];
                try {
                    if (jb.is_string())
                        m_config.projectileRedirect.customBaseAddress =
                            std::stoull(jb.get<std::string>(), nullptr, 16);
                    else if (jb.is_number_unsigned())
                        m_config.projectileRedirect.customBaseAddress = jb.get<uint64_t>();
                } catch (...) {
                    m_config.projectileRedirect.customBaseAddress = 0;
                }
            }
            m_config.projectileRedirect.key = jpr.value("key", 0);
            m_config.projectileRedirect.fov = jpr.value("fov", 15.0f);
            m_config.projectileRedirect.smoothness = jpr.value("smoothness", 5.0f);
            m_config.projectileRedirect.targetPlayers = jpr.value("targetPlayers", true);
            m_config.projectileRedirect.targetNPCs = jpr.value("targetNPCs", false);
            m_config.projectileRedirect.visibleOnly = jpr.value("visibleOnly", true);
            m_config.projectileRedirect.bone = jpr.value("bone", 0);
            m_config.projectileRedirect.maxRedirectDist = jpr.value("maxRedirectDist", 1000.0f);
            m_config.projectileRedirect.maxTargetDistance = jpr.value("maxTargetDistance", 100.0f);
            m_config.projectileRedirect.silentCamera = jpr.value("silentCamera", true);
            m_config.projectileRedirect.targetZOffset = jpr.value("targetZOffset", 0.0f);
            m_config.projectileRedirect.showFOVCircle = jpr.value("showFOVCircle", false);
            // The UI exposes one activation choice: key == 0 means Always Active,
            // and any nonzero key means that key must be held. Older configs can
            // contain a stale requireKey=false alongside a selected key (for
            // example key=Alt); honor the visible key choice when loading them.
            m_config.projectileRedirect.requireKey = m_config.projectileRedirect.key != 0;
        }

        if (j.contains("performance")) {
            auto& jp = j["performance"];
            m_config.performance.lowFootprintMode = jp.value("lowFootprintMode", false);
            m_config.performance.scanIntervalMs = jp.value("scanIntervalMs", 500);
            m_config.performance.positionUpdateIntervalMs = jp.value("positionUpdateIntervalMs", 16);
            m_config.performance.batchMemoryReads = jp.value("batchMemoryReads", true);
            m_config.performance.disableExpensiveESP = jp.value("disableExpensiveESP", false);
            m_config.performance.maxEntitiesProcessed = jp.value("maxEntitiesProcessed", 128);
            m_config.performance.reduceScanThreadPriority = jp.value("reduceScanThreadPriority", true);
        }

        if (j.contains("patterns")) {
            auto& jp = j["patterns"];
            m_config.gworldPattern = jp.value("gworld", "");
            m_config.gnamesPattern = jp.value("gnames", "");
            m_config.gobjectsPattern = jp.value("gobjects", "");
        }

        if (j.contains("offsets")) {
            auto& jo = j["offsets"];
            auto parseHex = [](const std::string& s) -> uintptr_t {
                if (s.empty()) return 0;
                if (s.front() == '-' || s.front() == '+')
                    throw std::runtime_error("Offsets must be unsigned hexadecimal values");
                size_t consumed = 0;
                const auto value = std::stoull(s, &consumed, 16);
                if (consumed != s.size() || value > (std::numeric_limits<uintptr_t>::max)())
                    throw std::runtime_error("Invalid hexadecimal offset: " + s);
                return static_cast<uintptr_t>(value);
            };
            if (jo.contains("gworld")) m_config.gworldOffset = parseHex(jo["gworld"].get<std::string>());
            if (jo.contains("gnames")) m_config.gnamesOffset = parseHex(jo["gnames"].get<std::string>());
            if (jo.contains("gobjects")) m_config.gobjectsOffset = parseHex(jo["gobjects"].get<std::string>());
            if (jo.contains("ue_version")) m_config.ueVersion = jo["ue_version"].get<std::string>();
        }

        spdlog::info("Config loaded from {}", m_filepath);
        m_loaded = true;
        return true;
    } catch (const std::exception& e) {
        m_config = previous;
        spdlog::error("Failed to parse config {}: {}", m_filepath, e.what());
        return false;
    }
}

void Config::Save() const {
    nlohmann::json j;

    j["filter"] = {
        {"showPlayers", m_config.filter.showPlayers},
        {"showLoot", m_config.filter.showLoot},
        {"showNPCs", m_config.filter.showNPCs},
        {"showPortals", m_config.filter.showPortals},
        {"showCorpses", m_config.filter.showCorpses},
        {"maxDistance", m_config.filter.maxDistance},
        {"maxPlayerDistance", m_config.filter.maxPlayerDistance},
        {"minLootRarity", static_cast<int>(m_config.filter.minLootRarity)},
        {"showHealthBars", m_config.filter.showHealthBars},
        {"showDamageNumbers", m_config.filter.showDamageNumbers},
        {"showDistance", m_config.filter.showDistance},
        {"showSnapLines", m_config.filter.showSnapLines},
        {"showGhostPlayers", m_config.filter.showGhostPlayers},
        {"minMonsterGrade", m_config.filter.minMonsterGrade}
    };

    j["visuals"] = {
        {"playerColor", {m_config.visuals.playerColor[0], m_config.visuals.playerColor[1],
                         m_config.visuals.playerColor[2], m_config.visuals.playerColor[3]}},
        {"teamColor", {m_config.visuals.teamColor[0], m_config.visuals.teamColor[1],
                       m_config.visuals.teamColor[2], m_config.visuals.teamColor[3]}},
        {"npcColor", {m_config.visuals.npcColor[0], m_config.visuals.npcColor[1],
                      m_config.visuals.npcColor[2], m_config.visuals.npcColor[3]}},
        {"fontSize", m_config.visuals.fontSize},
        {"boxStyle", m_config.visuals.boxStyle},
        {"radarRadius", m_config.visuals.radarRadius},
        {"radarRangeMeters", m_config.visuals.radarRangeMeters},
        {"minimapRotating", m_config.visuals.minimapRotating}
    };

    j["updateRateFps"] = m_config.updateRateFps;
    j["toggleMenuKey"] = m_config.toggleMenuKey;
    j["externalMode"] = m_config.externalMode;

    {
        char baseBuf[24];
        std::snprintf(baseBuf, sizeof(baseBuf), "0x%llX",
                      static_cast<unsigned long long>(m_config.projectileRedirect.customBaseAddress));
        j["projectileRedirect"] = {
            {"enabled", m_config.projectileRedirect.enabled},
            {"customBaseAddress", std::string(baseBuf)},
            {"key", m_config.projectileRedirect.key},
            {"fov", m_config.projectileRedirect.fov},
            {"smoothness", m_config.projectileRedirect.smoothness},
            {"targetPlayers", m_config.projectileRedirect.targetPlayers},
            {"targetNPCs", m_config.projectileRedirect.targetNPCs},
            {"visibleOnly", m_config.projectileRedirect.visibleOnly},
            {"bone", m_config.projectileRedirect.bone},
            {"maxRedirectDist", m_config.projectileRedirect.maxRedirectDist},
            {"maxTargetDistance", m_config.projectileRedirect.maxTargetDistance},
            {"requireKey", m_config.projectileRedirect.requireKey},
            {"silentCamera", m_config.projectileRedirect.silentCamera},
            {"targetZOffset", m_config.projectileRedirect.targetZOffset},
            {"showFOVCircle", m_config.projectileRedirect.showFOVCircle}
        };
    }

    j["performance"] = {
        {"lowFootprintMode", m_config.performance.lowFootprintMode},
        {"scanIntervalMs", m_config.performance.scanIntervalMs},
        {"positionUpdateIntervalMs", m_config.performance.positionUpdateIntervalMs},
        {"batchMemoryReads", m_config.performance.batchMemoryReads},
        {"disableExpensiveESP", m_config.performance.disableExpensiveESP},
        {"maxEntitiesProcessed", m_config.performance.maxEntitiesProcessed},
        {"reduceScanThreadPriority", m_config.performance.reduceScanThreadPriority}
    };

    j["patterns"] = {
        {"gworld", m_config.gworldPattern},
        {"gnames", m_config.gnamesPattern},
        {"gobjects", m_config.gobjectsPattern}
    };

    // Format offsets as hex strings
    auto toHex = [](uintptr_t val) -> std::string {
        char buf[32];
        snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(val));
        return buf;
    };

    j["offsets"] = {
        {"gworld", toHex(m_config.gworldOffset)},
        {"gnames", toHex(m_config.gnamesOffset)},
        {"gobjects", toHex(m_config.gobjectsOffset)},
        {"ue_version", m_config.ueVersion}
    };

    std::ofstream file(m_filepath);
    if (file.is_open()) {
        file << j.dump(2);
        spdlog::info("Config saved to {}", m_filepath);
    } else {
        spdlog::error("Failed to save config to {}", m_filepath);
    }
}
