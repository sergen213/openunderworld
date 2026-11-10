// Chapter sequence, level ends, pickups, script string constants. Evidence: docs/GAMEPLAY.md "Progression".
#include "game/Progress.hpp"

#include "formats/Formats.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

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

const ChapterRewards& chapterRewards(int chapter) {
    // In reward-table order (the index in each reward intro's data); U(n) = the treasure urn n gives birth to.
    constexpr auto U = [](int32_t urn) { return formats::bornUid(urn); };
    static const std::array<ChapterRewards, 8> kRewards{{
        {},
        {321,
         {U(318), U(317), U(316), U(315), U(314), 313, 312, U(311), U(310), U(95), U(94), U(145),
          U(144), U(635), U(595), U(679), 678, U(677), 794, 853, 852, 851, 850, 849, 848, U(7)}},
        {2588,
         {3838, 3839, 3840, 3841, U(3685), U(3105), U(2461), 2007, 2116, 2773, 2508, 3227, 3228,
          U(3225), U(3226), 2839, 2840, U(2838), U(2111), 2112, 2113, 2114, 2115, 1898, 1899,
          U(1909), 2601, 2602, 2603, 2604}},
        {4757,
         {U(4306), U(4305), U(4749), 4754, 4751, 4753, U(4750), 4752, 4643, 4644, U(4700), 4701,
          U(4601)}},
        {5496,
         {U(6617), U(6616), U(6615), U(6614), U(6613), U(6612), U(6611), U(6116), U(6115), U(6114),
          U(6113), U(6112), U(6111), U(6110), U(6109), U(6108), U(6107), 5776, 5775, 5774, 5773,
          5772, 5771, 5770, U(5769), U(5768), U(5767), U(5766), U(5765), U(5764), 5490, 5489, 5488,
          U(5487), U(5665), U(5664), U(5666), U(5667), U(4961), U(4960), U(4959), U(4965), U(4964),
          U(4963), U(4966), U(4962), U(4967), U(5325), U(5324), U(6056)}},
        {7280,
         {U(7429), U(7428), U(7427), U(7426), U(7425), 7430, U(7272), U(7271), U(7270), U(7269),
          U(7268), U(7267), 7276, 7275, 7274, 7273, 7167, U(7166), U(7165), U(7164), 7168, U(6739),
          U(6735), U(6738), U(6734), U(6737), U(6736), 6742, 6741, 6740}},
        {},
        {8936,
         {U(9197), U(9195), U(9194), U(9196), 9209, 9208, 9207, 9206, 9205, 9204, 9203, 9202, 9201,
          9200, 9199, 9198, 9165, 9164, 9141, 9140, 9139, 9138, U(8930), U(8928), U(8927), U(8926),
          U(8929), U(8925), U(8924), 8931}},
    }};
    return kRewards[size_t(std::min(std::clamp(chapter, 0, 8), 7))]; // the epilogue (8) is the Arctic Sea's
}

int RewardTable::total(int category, int type) const {
    int n = 0;
    for (const auto& g : groups)
        if (g.category == category)
            for (const uint32_t r : g.rewards) n += r < types.size() && (type == 0 || types[r] == type);
    return n;
}

std::optional<RewardTable> loadRewardTable(const cdc::BigfileSet& bigfile) {
    auto bytes = bigfile.read("pc-w\\generalbank.drm");
    if (!bytes) return std::nullopt;
    const cdc::Drm drm = cdc::parseDrm(std::move(*bytes));
    const auto rd = [](const cdc::Section& s, size_t o, auto& v) {
        if (o + sizeof(v) > s.data.size()) return false;
        std::memcpy(&v, s.data.data() + o, sizeof(v));
        return true;
    };
    // GlobalInfo (tru.exe DAT_00e7ee50) = generalbank's object data (ObjectBase +0xC8); +0x194 = the table's DTP id
    const auto base = formats::objectBaseSection(drm);
    if (!base) return std::nullopt;
    const auto info = cdc::internalPointer(drm.sections[*base], 0xC8);
    uint32_t id = 0;
    if (!info || !rd(drm.sections[*base], *info + 0x194, id)) return std::nullopt;
    for (const auto& t : drm.sections) {
        if (t.type != cdc::SectionType::DtpData || t.id != id) continue;
        RewardTable out;
        uint32_t count = 0, groups = 0;
        const auto rewards = cdc::internalPointer(t, 4), list = cdc::internalPointer(t, 0x14);
        if (!rd(t, 0, count) || !rd(t, 0x10, groups) || !rewards || !list || count > 2048 || groups > 256) return std::nullopt;
        for (uint32_t i = 0; i < count; ++i) {
            uint16_t type = 0;
            if (!rd(t, *rewards + 8 * i, type)) return std::nullopt;
            out.types.push_back(type);
        }
        for (uint32_t g = 0; g < groups; ++g) {
            RewardTable::Group group;
            uint32_t n = 0;
            const auto ids = cdc::internalPointer(t, *list + 0x10 * g + 0xC);
            if (!rd(t, *list + 0x10 * g, group.category) || !rd(t, *list + 0x10 * g + 8, n) || n > count || (n && !ids)) return std::nullopt;
            group.rewards.resize(n);
            for (uint32_t k = 0; k < n; ++k)
                if (!rd(t, *ids + 4 * k, group.rewards[k])) return std::nullopt;
            out.groups.push_back(std::move(group));
        }
        if (!rd(t, 0x58, out.heal) || !rd(t, 0x64, out.headroom)) return std::nullopt;
        return out;
    }
    return std::nullopt;
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
