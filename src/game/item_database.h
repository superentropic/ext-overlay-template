#pragma once
#include "core/config.h"
#include <string>
#include <unordered_map>

struct ItemInfo {
    std::string displayName;
    ItemRarity rarity = ItemRarity::COMMON;
    std::string category; // Weapon, Armor, Consumable, Accessory, etc.
};

class ItemDatabase {
public:
    void LoadFromFile(const std::string& filepath);

    const ItemInfo* Lookup(const std::string& className) const;

    // Add/update an entry at runtime (useful for discovery)
    void AddEntry(const std::string& className, const ItemInfo& info);

    void SaveToFile(const std::string& filepath) const;

    size_t Size() const { return m_items.size(); }

private:
    std::unordered_map<std::string, ItemInfo> m_items;
};
