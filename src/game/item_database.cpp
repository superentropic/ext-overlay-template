#include "item_database.h"
#include <fstream>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

void ItemDatabase::LoadFromFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        spdlog::info("Item database not found: {}. Starting empty.", filepath);
        return;
    }

    try {
        nlohmann::json j;
        file >> j;

        for (auto& [key, val] : j.items()) {
            // Skip non-object entries (e.g. "_comment" strings)
            if (!val.is_object()) continue;

            ItemInfo info;
            info.displayName = val.value("displayName", key);
            info.rarity = static_cast<ItemRarity>(val.value("rarity", 2));
            info.category = val.value("category", "Unknown");
            m_items[key] = info;
        }

        spdlog::info("Loaded {} items from database", m_items.size());
    } catch (const std::exception& e) {
        spdlog::error("Failed to parse item database: {}", e.what());
    }
}

const ItemInfo* ItemDatabase::Lookup(const std::string& className) const {
    auto it = m_items.find(className);
    if (it != m_items.end())
        return &it->second;
    return nullptr;
}

void ItemDatabase::AddEntry(const std::string& className, const ItemInfo& info) {
    m_items[className] = info;
}

void ItemDatabase::SaveToFile(const std::string& filepath) const {
    nlohmann::json j;

    for (const auto& [key, info] : m_items) {
        j[key] = {
            {"displayName", info.displayName},
            {"rarity", static_cast<int>(info.rarity)},
            {"category", info.category}
        };
    }

    std::ofstream file(filepath);
    if (file.is_open()) {
        file << j.dump(2);
        spdlog::info("Saved {} items to database", m_items.size());
    }
}
