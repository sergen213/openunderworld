// Chapter sequence, level ends, pickups, script string constants. Evidence: docs/GAMEPLAY.md "Progression".
#include "game/Progress.hpp"

#include <algorithm>
#include <cctype>

namespace ou::game {

int chapterIndex(std::string_view startUnit) {
    for (size_t i = 0; i < kChapters.size(); ++i)
        if (kChapters[i].unit == startUnit) return int(i);
    return -1;
}

int chapterOfUnit(std::string_view unit) {
    if (unit == kChapters[8].unit) return 8;
    if (unit.size() > 2 && unit[0] == 'l' && unit[1] >= '1' && unit[1] <= '7' && unit[2] == '_') return unit[1] - '0';
    return -1;
}

std::optional<int> nextChapter(int current, const std::vector<std::string>& startUnits) {
    std::optional<int> best;
    for (const auto& u : startUnits)
        if (const int c = chapterIndex(u); c > current && (!best || c < *best)) best = c;
    return best;
}

std::optional<PickupKind> pickupKind(std::string_view n) {
    if (n.find("_rw_artifact") != std::string_view::npos) return PickupKind::Treasure;
    if (n.find("_rw_treasure") != std::string_view::npos) return PickupKind::Relic;
    if (n.find("_rw_shard") != std::string_view::npos) return PickupKind::HealthPack;
    return std::nullopt;
}

int16_t pickupStringId(PickupKind k) {
    switch (k) {
    case PickupKind::Treasure: return 1251;
    case PickupKind::Relic: return 1252;
    case PickupKind::HealthPack: return 1274;
    }
    return 0;
}

const char* pickupKindName(PickupKind k) {
    switch (k) {
    case PickupKind::Treasure: return "treasure";
    case PickupKind::Relic: return "relic";
    case PickupKind::HealthPack: return "healthpack";
    }
    return "?";
}

std::vector<std::string> scriptStrings(const cdc::Drm& drm, uint32_t sectionId) {
    std::vector<std::string> out;
    for (const auto& s : drm.sections) {
        if (s.type != cdc::SectionType::Script || s.id != sectionId) continue;
        const auto& d = s.data;
        auto word = [](uint8_t c) { return std::isalnum(c) || c == '_'; };
        for (size_t i = 0; i < d.size();) {
            size_t j = i;
            while (j < d.size() && word(d[j])) ++j;
            if (j < d.size() && d[j] == 0 && j - i >= 4 && (i == 0 || d[i - 1] == 0 || !std::isprint(d[i - 1]))) {
                std::string w(reinterpret_cast<const char*>(d.data() + i), j - i);
                std::transform(w.begin(), w.end(), w.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                out.push_back(std::move(w));
            }
            i = j + 1;
        }
        break;
    }
    return out;
}

} // namespace ou::game
