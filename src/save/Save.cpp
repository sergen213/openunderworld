// Save games: World capture/apply, versioned JSON + CRC32 header, atomic file replace with a backup.
#include "save/Save.hpp"

#include "config/Config.hpp"

#include <zlib.h>

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <sstream>

namespace ou::save {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

constexpr std::string_view kMagic = "OUSAVE";

json vec(game::vec3 v) { return json::array({v.x, v.y, v.z}); }
game::vec3 vec(const json& j) {
    if (!j.is_array() || j.size() != 3) throw SaveError("expected [x, y, z]");
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}

game::State stateFromName(const std::string& name) {
    // stateName() returns "unknown" past the last state.
    for (int i = 0; i < 256; ++i) {
        const auto n = game::stateName(game::State(i));
        if (n == "unknown") break;
        if (n == name) return game::State(i);
    }
    throw SaveError("unknown player state '" + name + "'");
}

json toJson(const SaveGame& s) {
    const auto& p = s.player;
    json ledge = nullptr;
    if (!s.ledgeUnit.empty())
        ledge = {{"unit", s.ledgeUnit}, {"index", s.ledgeIndex}, {"seg", p.ledgeSeg}, {"t", p.ledgeT}, {"side", p.ledgeSide}};
    json water = nullptr;
    if (!s.waterUnit.empty()) water = {{"unit", s.waterUnit}, {"index", s.waterIndex}};
    json reveal = json::object();
    for (const auto& [unit, points] : s.reveal) {
        json& a = reveal[unit] = json::array();
        for (const auto& q : points) a.push_back(vec(q));
    }
    json cp = nullptr;
    if (s.checkpoint) cp = {{"unit", s.checkpoint->unit}, {"id", s.checkpoint->id}};
    return {
        {"unit", s.unit},
        {"ticks", s.ticks},
        {"player", {{"state", std::string(game::stateName(p.state))}, {"pos", vec(p.pos)}, {"yaw", p.yaw}, {"vel", vec(p.vel)},
                    {"clip", p.clip}, {"clipTime", p.clipTime}, {"clipRate", p.clipRate}, {"loop", p.loop},
                    {"ledge", ledge}, {"runJump", p.runJump}, {"regrabCooldown", p.regrabCooldown},
                    {"water", water}, {"pitch", p.pitch}, {"fallStartZ", p.fallStartZ}, {"controller", s.controller}}},
        {"camera", {{"yaw", s.cameraYaw}, {"pitch", s.cameraPitch}}},
        {"runtime", s.runtime},
        {"checkpoint", cp},
        {"collected", s.collected},
        {"chapter", s.chapter},
        {"found", s.found},
        {"grenades", s.grenades ? json(*s.grenades) : json(nullptr)},
        {"flashlight", s.flashlight},
        {"treasureHunt", s.treasureHunt},
        {"outfit", s.outfit},
        {"health", s.health ? json(*s.health) : json(nullptr)},
        {"tailoring", s.tailoring ? json{s.tailoring->enemyHealth, s.tailoring->ammo, s.tailoring->laraHealth, s.tailoring->savingGrab,
                                         s.tailoring->unknown}
                                  : json(nullptr)},
        {"script", s.script},
        {"enemies", s.enemies},
        {"vehicles", s.vehicles},
        {"combat", s.combat},
        {"objects", s.objects},
        {"reveal", reveal},
    };
}

// Version 1 (development draft, docs/SAVES.md): no camera/collected/script, checkpoint was a bare intro id in
// the save's unit, player had only state/pos/yaw.
json migrate1to2(json j) {
    if (j.contains("checkpoint") && j["checkpoint"].is_number_integer())
        j["checkpoint"] = {{"unit", j.at("unit")}, {"id", j["checkpoint"]}};
    return j;
}

SaveGame fromJson(const json& j) {
    SaveGame s;
    s.unit = j.at("unit").get<std::string>();
    if (s.unit.empty()) throw SaveError("empty unit");
    s.ticks = j.value("ticks", uint64_t(0));
    const json& p = j.at("player");
    auto& ps = s.player;
    ps.state = stateFromName(p.at("state").get<std::string>());
    ps.pos = vec(p.at("pos"));
    ps.yaw = p.at("yaw").get<float>();
    if (p.contains("vel")) ps.vel = vec(p["vel"]);
    ps.clip = p.value("clip", std::string("ng_idle"));
    ps.clipTime = p.value("clipTime", 0.0);
    ps.clipRate = p.value("clipRate", 1.0f);
    ps.loop = p.value("loop", true);
    ps.runJump = p.value("runJump", false);
    ps.regrabCooldown = p.value("regrabCooldown", 0.0f);
    ps.pitch = p.value("pitch", 0.0f);
    ps.fallStartZ = p.value("fallStartZ", ps.pos.z); // older saves: falls count from where she stands
    s.controller = p.value("controller", json(nullptr));
    if (!s.controller.is_null() && !s.controller.is_object()) throw SaveError("player controller is not an object");
    if (const auto w = p.find("water"); w != p.end() && !w->is_null())
        s.waterUnit = w->at("unit").get<std::string>(), s.waterIndex = w->at("index").get<int>();
    if (const auto l = p.find("ledge"); l != p.end() && !l->is_null()) {
        s.ledgeUnit = l->at("unit").get<std::string>();
        s.ledgeIndex = l->at("index").get<int>();
        ps.ledgeSeg = l->at("seg").get<int>();
        ps.ledgeT = l->at("t").get<float>();
        ps.ledgeSide = l->at("side").get<float>();
    }
    if (const auto c = j.find("camera"); c != j.end()) {
        s.cameraYaw = c->at("yaw").get<float>();
        s.cameraPitch = c->at("pitch").get<float>();
        s.runtime = j.value("runtime", json(nullptr)); // written beside the camera angles
    } else {
        s.cameraYaw = ps.yaw;
    }
    if (const auto c = j.find("checkpoint"); c != j.end() && !c->is_null())
        s.checkpoint = game::CheckpointRef{c->at("unit").get<std::string>(), c->at("id").get<int32_t>()};
    s.collected = j.value("collected", std::set<int32_t>{});
    s.chapter = j.value("chapter", std::max(0, game::chapterOfUnit(s.unit)));
    s.found = j.value("found", std::array<int, 3>{});
    if (const auto g = j.find("grenades"); g != j.end() && !g->is_null()) s.grenades = std::max(0, g->get<int>());
    s.flashlight = j.value("flashlight", false);
    s.treasureHunt = j.value("treasureHunt", false);
    s.outfit = j.value("outfit", std::string{});
    if (s.outfit.size() > 64 || s.outfit.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos)
        throw SaveError("bad outfit " + s.outfit);
    if (const auto h = j.find("health"); h != j.end() && !h->is_null()) {
        s.health = h->get<float>();
        if (!(*s.health > 0 && *s.health < 1e6f)) throw SaveError("bad health " + h->dump());
    }
    if (const auto t = j.find("tailoring"); t != j.end() && !t->is_null()) { // levels: enemy health, ammo, Lara's health, grab, ?
        const auto v = t->get<std::vector<int>>();
        if (v.size() != 5 || std::any_of(v.begin(), v.end(), [](int l) { return l < 0 || l > 2; })) throw SaveError("bad tailoring " + t->dump());
        s.tailoring = game::Tailoring{v[0], v[1], v[2], v[3], v[4]};
    }
    if (s.chapter < 0 || s.chapter >= int(game::kChapters.size())) throw SaveError("bad chapter " + std::to_string(s.chapter));
    s.script = j.value("script", json::object());
    if (!s.script.is_object()) throw SaveError("script state is not an object");
    s.enemies = j.value("enemies", json(nullptr));
    if (!s.enemies.is_null() && !s.enemies.is_object()) throw SaveError("enemy state is not an object");
    s.vehicles = j.value("vehicles", json(nullptr));
    if (!s.vehicles.is_null() && !s.vehicles.is_object()) throw SaveError("vehicle state is not an object");
    s.combat = j.value("combat", json(nullptr));
    if (!s.combat.is_null() && !s.combat.is_object()) throw SaveError("combat state is not an object");
    s.objects = j.value("objects", json(nullptr));
    if (!s.objects.is_null() && !s.objects.is_object()) throw SaveError("object state is not an object");
    if (const auto r = j.find("reveal"); r != j.end() && r->is_object())
        for (const auto& [unit, points] : r->items())
            for (const auto& q : points) s.reveal[unit].push_back(vec(q));
    return s;
}

void fsyncPath(const fs::path& p, int flags) {
    const int fd = ::open(p.c_str(), flags);
    if (fd < 0) throw SaveError("cannot open " + p.string() + " for fsync");
    const int r = ::fsync(fd);
    ::close(fd);
    if (r != 0) throw SaveError("fsync " + p.string() + " failed");
}

json markupRef(const game::World& w, int index) {
    const auto owner = w.markupOwner(index);
    return owner ? json{owner->first, owner->second} : json(nullptr);
}

json vec2j(game::vec2 v) { return json::array({v.x, v.y}); }
game::vec2 vec2j(const json& j) { return {j.at(0).get<float>(), j.at(1).get<float>()}; }

// player.controller: the PlayerState fields the player block above leaves out. `p` in the world frame.
json controllerJson(const game::World& w, const game::PlayerState& p, game::vec3 offset) {
    const auto& h = p.hurdle;
    json bends = json::array();
    for (size_t i = 0; i < p.ropeBends; ++i) bends.push_back({vec(p.ropeBend[i] - offset), p.ropeBendClear[i]});
    return {{"clipMix", p.clipMix}, {"mix", p.mix}, {"mirror", p.mirror},
            {"hurdle", {int(h.next), h.mix, h.alt, h.draw, vec2j(h.drawOff), vec(h.edge - offset), vec2j(h.out), h.h, h.endZ - offset.z,
                        markupRef(w, h.farLedge), h.farSeg, h.farT, h.farSide}},
            {"savingGrab", p.savingGrab}, {"flight", p.flight}, {"fallHeight", p.fallHeight},
            {"climb", {markupRef(w, p.climbHold), p.climbStance, p.climbWarp, vec(p.climbNormal), vec(p.climbFrom - offset),
                       vec(p.climbTo - offset), p.climbYawFrom, p.climbYawTo}},
            {"grapple", {p.grappleUid, vec(p.grappleAnchor - offset), p.ropeLength, int(p.grappleKind), bends, p.ropeMax}},
            {"sprintUsed", p.sprintUsed}, {"polePara", p.polePara},
            {"wallJump", {vec(p.jumpStart - offset), p.jumpTime, p.jumpAimed, p.wallJumps, p.wallTime,
                          vec(p.jumpFromLedge ? p.jumpLedge - offset : game::vec3(0)), p.jumpFromLedge}},
            {"balance", {p.balance.build, p.balance.aboutFall, p.balance.grace, p.balance.unbal, p.balance.bal, p.balance.notImprove,
                         p.balance.full, p.balance.clock, p.balance.lastFlip, p.balance.period, p.balance.ref, p.balance.stickYaw,
                         p.balance.blend, p.balance.lean, p.balance.side, p.balance.osc, p.balance.fall}}};
}

// Into `p` (the save's unit at offset 0, as apply loads it). Throws json / SaveError on a malformed block.
void applyController(const json& c, const game::World& w, game::PlayerState& p) {
    const auto ref = [&](const json& r) {
        if (r.is_null()) return -1;
        const int i = w.markupIndex(r.at(0).get<std::string>(), r.at(1).get<int>());
        if (i < 0 && !r.at(0).get<std::string>().starts_with('@')) throw SaveError("markup " + r.dump() + " is not loaded");
        return i;
    };
    p.clipMix = c.at("clipMix").get<std::string>(), p.mix = c.at("mix").get<float>(), p.mirror = c.value("mirror", false);
    const auto& h = c.at("hurdle");
    auto& d = p.hurdle;
    const int next = h.at(0).get<int>();
    if (next < 0 || next > game::PlayerState::Hurdle::Down) throw SaveError("bad hurdle " + h.dump());
    d.next = game::PlayerState::Hurdle::Next(next), d.mix = h.at(1), d.alt = h.at(2), d.draw = h.at(3), d.drawOff = vec2j(h.at(4));
    d.edge = vec(h.at(5)), d.out = vec2j(h.at(6)), d.h = h.at(7), d.endZ = h.at(8), d.farLedge = ref(h.at(9));
    d.farSeg = h.at(10), d.farT = h.at(11), d.farSide = h.at(12);
    p.savingGrab = c.at("savingGrab"), p.flight = c.at("flight"), p.fallHeight = c.at("fallHeight");
    const auto& cl = c.at("climb");
    p.climbHold = ref(cl.at(0)), p.climbStance = cl.at(1), p.climbWarp = cl.at(2), p.climbNormal = vec(cl.at(3));
    p.climbFrom = vec(cl.at(4)), p.climbTo = vec(cl.at(5)), p.climbYawFrom = cl.at(6), p.climbYawTo = cl.at(7);
    const auto& g = c.at("grapple");
    const int kind = g.at(3).get<int>();
    if (kind < 0 || kind > int(game::GrappleKind::Mounted)) throw SaveError("bad grapple kind " + g.dump());
    p.grappleUid = g.at(0), p.grappleAnchor = vec(g.at(1)), p.ropeLength = g.at(2), p.grappleKind = game::GrappleKind(kind);
    p.ropeBends = 0; // older saves: a straight rope
    if (g.size() > 4)
        for (const auto& b : g.at(4)) {
            if (p.ropeBends == game::kRopeBends) throw SaveError("too many rope bends " + g.dump());
            p.ropeBend[p.ropeBends] = vec(b.at(0)), p.ropeBendClear[p.ropeBends] = b.at(1).get<uint8_t>();
            ++p.ropeBends;
        }
    p.ropeMax = g.size() > 5 ? g.at(5).get<float>() : 0.0f; // older saves: g_grapple's rope
    p.sprintUsed = c.at("sprintUsed");
    p.polePara = c.value("polePara", false); // older saves: the perpendicular hang
    p.jumpStart = p.pos, p.jumpTime = 0, p.jumpAimed = false, p.wallJumps = 0, p.wallTime = 0; // older saves: no chimney jump
    p.jumpLedge = game::vec3(0), p.jumpFromLedge = false;
    if (const auto wj = c.find("wallJump"); wj != c.end()) {
        if (!wj->is_array() || wj->size() != 7) throw SaveError("bad wallJump " + wj->dump());
        p.jumpStart = vec(wj->at(0)), p.jumpTime = wj->at(1), p.jumpAimed = wj->at(2), p.wallJumps = wj->at(3).get<uint8_t>();
        p.wallTime = wj->at(4), p.jumpLedge = vec(wj->at(5)), p.jumpFromLedge = wj->at(6);
    }
    p.balance = {}; // older saves: steady on a beam
    if (const auto b = c.find("balance"); b != c.end()) {
        if (!b->is_array() || b->size() != 17) throw SaveError("bad balance " + b->dump());
        auto& d = p.balance;
        d.build = b->at(0), d.aboutFall = b->at(1), d.grace = b->at(2), d.unbal = b->at(3), d.bal = b->at(4), d.notImprove = b->at(5);
        d.full = b->at(6), d.clock = b->at(7), d.lastFlip = b->at(8), d.period = b->at(9), d.ref = b->at(10), d.stickYaw = b->at(11);
        d.blend = b->at(12), d.lean = b->at(13), d.side = b->at(14).get<int8_t>(), d.osc = b->at(15).get<int8_t>(), d.fall = b->at(16);
    }
}

} // namespace

SaveGame capture(const game::World& world) {
    if (world.units().empty()) throw SaveError("capture needs a streaming World");
    const auto& unit = world.currentUnit();
    SaveGame s;
    s.unit = unit.data.name;
    s.player = world.player().state();
    s.player.pos -= unit.offset;
    s.player.fallStartZ -= unit.offset.z;
    if (const auto owner = world.markupOwner(s.player.ledge)) s.ledgeUnit = owner->first, s.ledgeIndex = owner->second;
    s.controller = controllerJson(world, world.player().state(), unit.offset);
    if (const auto owner = world.markupOwner(s.player.water)) s.waterUnit = owner->first, s.waterIndex = owner->second;
    s.player.ledge = s.player.water = -1;
    s.cameraYaw = world.camera().yaw;
    s.cameraPitch = world.camera().pitch;
    s.runtime = world.serializeRuntime();
    s.checkpoint = world.checkpoint();
    s.collected = world.collected;
    s.chapter = world.chapter;
    s.found = world.found;
    s.grenades = world.grenades, s.flashlight = world.flashlight, s.treasureHunt = world.treasureHunt;
    s.outfit = world.levelOutfit;
    if (!world.health().dead()) s.health = world.health().value(); // [I] a save taken while dead loads at full
    s.tailoring = world.tailoring();
    s.script = world.script().serialize();
    s.enemies = world.enemies().serialize();
    s.vehicles = world.serializeVehicles();
    s.combat = world.serializeCombat();
    s.objects = world.serializeObjects();
    s.reveal = world.mapReveal;
    s.ticks = world.ticks();
    return s;
}

void apply(const SaveGame& save, game::World& world) {
    if (save.tailoring) world.setTailoring(*save.tailoring); // first: the units' enemies spawn with its health level
    world.setTicks(save.ticks); // before enterUnit: its stream timers count from the saved clock (waves run on it too)
    // An airborne save resumes at its checkpoint (else its unit's spawn), as tru.exe's loads always do (checkpoint
    // restore 0x556E90): one taken mid-fall holds no place to come down on. The chapter-1 start slot of older builds held
    // Lara on l1_cineseatop's set, 166000 units off the yacht, falling through the sky [user report].
    const bool airborne = save.player.state == game::State::Fall || save.player.state == game::State::Jump;
    if (airborne) {
        world.enterUnit(save.checkpoint ? save.checkpoint->unit : save.unit, std::nullopt, save.checkpoint ? save.checkpoint->id : 0);
    } else {
        world.enterUnit(save.unit, game::Spawn{save.player.pos, save.player.yaw});
        game::PlayerState p = save.player; // enterUnit put the save's unit at offset 0: positions apply as-is
        auto markup = [&](const std::string& unit, int index, const char* what) {
            if (unit.empty()) return -1;
            const int i = world.markupIndex(unit, index);
            // An object's markup ("@uid", World::tickObjectMarkup) exists once its collision is placed: a ledge on it that
            // is not there yet is dropped (her state handles a missing ledge as at a stream) rather than failing the load.
            if (i < 0 && unit.starts_with('@')) return -1;
            if (i < 0) throw SaveError(std::string(what) + " " + std::to_string(index) + " of " + unit + " is not loaded");
            return i;
        };
        p.ledge = markup(save.ledgeUnit, save.ledgeIndex, "ledge");
        if (p.ledge < 0 && !save.ledgeUnit.empty() &&
            (p.state == game::State::Hang || p.state == game::State::Shimmy || p.state == game::State::ClimbUp || p.state == game::State::PoleHang))
            p.state = game::State::Fall;
        p.water = markup(save.waterUnit, save.waterIndex, "water volume");
        if (!save.controller.is_null()) {
            try {
                applyController(save.controller, world, p);
            } catch (const json::exception& e) {
                throw SaveError(std::string("player controller: ") + e.what());
            }
        }
        world.restore(p, save.cameraYaw, save.cameraPitch);
        world.restoreRuntime(save.runtime);
    }
    world.setCheckpoint(save.checkpoint);
    world.collected = save.collected;
    world.chapter = save.chapter;
    world.found = save.found;
    if (save.grenades) world.grenades = *save.grenades;
    world.flashlight = save.flashlight;
    world.treasureHunt = save.treasureHunt;
    // tru.exe 0x5DE2B0: a load keeps the saved outfit (game state +0x20C), except l1_seatop forces lara_wetsuit_warm.
    // A chosen costume (World::costume, not saved) still wins.
    if (!save.outfit.empty()) world.levelOutfit = save.unit == "l1_seatop" ? "lara_wetsuit_warm" : save.outfit;
    world.wear(world.costume);
    world.script().restore(save.script);
    if (airborne) world.script().playerJumped(); // she resumes elsewhere: no plane crossings across the jump
    if (!save.script.is_object() || !save.script.contains("player")) // older saves: what the chapter starts with
        world.script().setPlayerObjects(game::chapterGear(save.chapter));
    if (!save.enemies.is_null()) {
        try {
            world.restoreEnemies(save.enemies);
        } catch (const std::runtime_error& e) {
            throw SaveError(e.what());
        }
    }
    if (!save.vehicles.is_null()) {
        try {
            world.restoreVehicles(save.vehicles);
        } catch (const std::exception& e) {
            throw SaveError(std::string("vehicles: ") + e.what());
        }
    }
    if (save.combat.is_null()) world.chapterLoadout(save.chapter); // older saves: what the chapter starts with
    else world.restoreCombat(save.combat);
    world.mapReveal = save.reveal;
    try {
        world.restoreObjects(save.objects);
    } catch (const std::runtime_error& e) {
        throw SaveError(e.what());
    }
    if (save.health) world.setHealth(*save.health);
}

std::string serialize(const SaveGame& save) {
    const std::string body = toJson(save).dump(1);
    char header[64];
    std::snprintf(header, sizeof header, "%.*s %d %08lx %zu\n", int(kMagic.size()), kMagic.data(), kVersion,
                  crc32(0, reinterpret_cast<const Bytef*>(body.data()), uInt(body.size())), body.size());
    return header + body;
}

SaveGame parse(std::string_view bytes) {
    const size_t nl = bytes.find('\n');
    if (nl == std::string_view::npos || nl > 64) throw SaveError("no save header");
    std::istringstream h{std::string(bytes.substr(0, nl))};
    std::string magic, crcHex;
    int version = 0;
    size_t size = 0;
    if (!(h >> magic >> version >> crcHex >> size) || magic != kMagic) throw SaveError("not an OpenUnderworld save");
    if (version < 1 || version > kVersion) throw SaveError("unsupported save version " + std::to_string(version));
    const std::string_view body = bytes.substr(nl + 1);
    if (body.size() != size)
        throw SaveError("body is " + std::to_string(body.size()) + " bytes, header says " + std::to_string(size) + " (truncated?)");
    const unsigned long crc = crc32(0, reinterpret_cast<const Bytef*>(body.data()), uInt(body.size()));
    if (std::strtoul(crcHex.c_str(), nullptr, 16) != crc) throw SaveError("checksum mismatch (corrupt save)");
    try {
        json j = json::parse(body);
        if (!j.is_object()) throw SaveError("body is not a JSON object");
        if (version == 1) j = migrate1to2(std::move(j));
        return fromJson(j);
    } catch (const json::exception& e) {
        throw SaveError(std::string("bad save data: ") + e.what());
    }
}

fs::path saveDir() { return config::dataHome() / "openunderworld" / "saves"; }

fs::path slotPath(std::string_view slot, const fs::path& dir) {
    if (slot.empty() || slot.size() > 32 ||
        slot.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string_view::npos)
        throw SaveError("bad save slot name '" + std::string(slot) + "'");
    return dir / (std::string(slot) + ".ousave");
}

void writeFile(const fs::path& path, std::string_view bytes) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) throw SaveError("cannot create " + path.parent_path().string() + ": " + ec.message());
    const fs::path tmp = path.string() + ".tmp", bak = path.string() + ".bak", bakTmp = path.string() + ".bak.tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(bytes.data(), std::streamsize(bytes.size()));
        f.close();
        if (!f) throw SaveError("cannot write " + tmp.string());
    }
    fsyncPath(tmp, O_RDONLY);
    // Backup: a second name for the current file, swapped in atomically (no window without a backup).
    if (fs::exists(path)) {
        fs::remove(bakTmp, ec);
        fs::create_hard_link(path, bakTmp, ec);
        if (!ec) fs::rename(bakTmp, bak, ec);
        if (ec) throw SaveError("cannot back up " + path.string() + ": " + ec.message());
    }
    fs::rename(tmp, path, ec);
    if (ec) throw SaveError("cannot replace " + path.string() + ": " + ec.message());
    fsyncPath(path.parent_path(), O_RDONLY | O_DIRECTORY);
}

SaveGame readFile(const fs::path& path, bool* usedBackup) {
    auto read = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        if (!f) throw SaveError("cannot open " + p.string());
        return parse(std::string(std::istreambuf_iterator<char>(f), {}));
    };
    if (usedBackup) *usedBackup = false;
    try {
        return read(path);
    } catch (const SaveError& e) {
        const fs::path bak = path.string() + ".bak";
        try {
            auto s = read(bak);
            if (usedBackup) *usedBackup = true;
            return s;
        } catch (const SaveError& e2) {
            throw SaveError(std::string(e.what()) + "; backup: " + e2.what());
        }
    }
}

Profile readProfile(const fs::path& dir) {
    std::ifstream f(dir / "profile.json");
    if (!f) return {};
    try {
        const json j = json::parse(f);
        Profile p{j.value("chapters", 0u), j.value("finished", false), j.value("treasures", false), j.value("relics", false),
                  j.value("outfit", std::string{})};
        if (p.outfit.size() > 64 || p.outfit.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos)
            p.outfit.clear();
        return p;
    } catch (const std::exception&) {
        return {};
    }
}

void writeProfile(const Profile& p, const fs::path& dir) {
    writeFile(dir / "profile.json", json{{"chapters", p.chapters}, {"finished", p.finished}, {"treasures", p.treasures}, {"relics", p.relics},
                                          {"outfit", p.outfit}}.dump());
}

} // namespace ou::save
