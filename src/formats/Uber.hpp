// UberObjectComposite_TR8 data stored in the object section's physical-class record (+0xC8).
#pragma once

#include "cdc/Cdc.hpp"

#include <optional>
#include <vector>

namespace ou::formats {

struct UberTarget {
    int32_t section = 0;
    uint32_t type = 0, payload = 0;
};
struct UberCommand {
    uint32_t id = 0;
    std::vector<UberTarget> targets;
};
struct UberObjectData {
    uint16_t objectClass = 0;
    std::vector<uint8_t> sectionModels;
    std::vector<UberCommand> commands;
};

// nullopt for non-uber objects. Throws cdc::FormatError on malformed uber data.
std::optional<UberObjectData> loadUberObject(const cdc::Drm& drm);

} // namespace ou::formats
