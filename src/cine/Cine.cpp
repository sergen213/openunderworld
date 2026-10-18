// CINE stream parsing and playback. Evidence: docs/formats/CINEMATICS.md.
#include "cine/Cine.hpp"

#include "script/Script.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ou::cine {

using cdc::FormatError;

namespace {

uint32_t rd32(std::span<const uint8_t> b, size_t o) {
    uint32_t v;
    std::memcpy(&v, b.data() + o, 4);
    return v;
}

struct Reader {
    std::span<const uint8_t> b;
    size_t p = 0;
    void need(uint64_t n) const {
        if (n > b.size() - p) throw FormatError("cine: truncated at " + std::to_string(p));
    }
    uint32_t u32() { need(4); const uint32_t v = rd32(b, p); p += 4; return v; }
    float f32() { const uint32_t v = u32(); float f; std::memcpy(&f, &v, 4); return f; }
    void skip(uint64_t n) { need(n); p += n; }
};

glm::vec3 lerp3(const std::vector<float>& a, const std::vector<float>& b, uint32_t c, float t) {
    return glm::mix(glm::vec3(a[c], a[c + 1], a[c + 2]), glm::vec3(b[c], b[c + 1], b[c + 2]), t);
}

glm::quat euler(const std::vector<float>& v, uint32_t c) { // R = Rz * Ry * Rx (0x49C6F0; camera survey in the doc)
    return glm::angleAxis(v[c + 2], glm::vec3(0, 0, 1)) * glm::angleAxis(v[c + 1], glm::vec3(0, 1, 0)) *
           glm::angleAxis(v[c], glm::vec3(1, 0, 0));
}

} // namespace

std::vector<uint8_t> demux(std::span<const uint8_t> mul) {
    // MultiplexStream: 2048-byte header, then packets {u32 type, u32 size, u32, u32, size bytes}; 1 = cinematic.
    if (mul.size() < 2048) throw FormatError("mul: truncated header");
    std::vector<uint8_t> out;
    for (size_t o = 2048; o + 16 <= mul.size();) {
        const uint32_t type = rd32(mul, o), size = rd32(mul, o + 4);
        if (size > mul.size() - o - 16) throw FormatError("mul: truncated packet");
        if (type == 1) out.insert(out.end(), mul.begin() + long(o) + 16, mul.begin() + long(o + 16 + size));
        o += 16 + size;
    }
    return out;
}

Header parseHeader(std::span<const uint8_t> cine) {
    Reader r{cine};
    r.need(12);
    if (std::memcmp(cine.data(), "ENIC", 4) != 0) throw FormatError("cine: bad magic");
    r.p = 4;
    Header h;
    h.version = r.u32();
    const uint32_t headerSize = r.u32(); // bytes after the magic + version
    r.need(64);
    h.unitName.assign(reinterpret_cast<const char*>(cine.data() + r.p), strnlen(reinterpret_cast<const char*>(cine.data() + r.p), 64));
    r.p += 64;
    h.unitId = r.u32();
    uint64_t ch = 0;
    auto count = [&](uint64_t perItem) { // sanity bound before allocating
        const uint32_t n = r.u32();
        r.need(n * perItem);
        return n;
    };
    for (uint32_t i = 0, n = count(12); i < n; ++i, ch += 9) {
        Marker m;
        m.unit = r.u32(), m.intro = int32_t(r.u32()), m.channel = r.u32();
        h.markers.push_back(m);
    }
    for (uint32_t i = 0, n = count(32); i < n; ++i) {
        Instance in;
        in.intro = int32_t(r.u32()), in.alt = int32_t(r.u32());
        in.bones = r.u32();
        in.channel = r.u32();
        r.need(64ull * in.bones);
        for (uint32_t b = 0; b < in.bones; ++b) { // 4x4 floats (0x453630), translation dropped by the exe
            float m[16];
            for (float& f : m) f = r.f32();
            glm::mat4 fix = glm::make_mat4(m);
            fix[0][3] = fix[1][3] = fix[2][3] = 0, fix[3] = glm::vec4(0, 0, 0, 1);
            in.boneFix.push_back(fix);
        }
        ch += 14ull * in.bones + 3;
        in.lights = count(24);
        in.lightChannel = r.u32();
        r.skip(24ull * in.lights); // {bone, type, flag, ?, ?, table} (0x454BD0 light setup)
        ch += 10ull * in.lights;
        in.vars = r.u32();
        in.varChannel = r.u32();
        ch += in.vars;
        h.instances.push_back(std::move(in));
    }
    for (uint32_t i = 0, n = count(4); i < n; ++i, ch += 16) h.cameras.push_back(r.u32());
    h.eventTarget = r.u32();
    for (uint32_t i = 0, n = count(8); i < n; ++i, ++ch) {
        Event e;
        e.id = r.u32(), e.channel = r.u32();
        h.events.push_back(e);
    }
    r.skip(4);
    if (r.p < uint64_t(headerSize) + 8)
        for (auto& in : h.instances) { // 7 dwords per instance
            glm::vec3 p, e;
            for (int k = 0; k < 3; ++k) p[k] = r.f32();
            for (int k = 0; k < 3; ++k) e[k] = r.f32();
            in.endPos = p, in.endRot = e, in.endUnit = r.u32();
        }
    if (r.p != uint64_t(headerSize) + 8) throw FormatError("cine: header size mismatch");
    if (ch > (1u << 20)) throw FormatError("cine: implausible channel count");
    h.channels = uint32_t(ch);
    h.framesOffset = r.p;
    return h;
}

Decoder::Decoder(std::span<const uint8_t> cine, const Header& h)
    : b_(cine), p_(h.framesOffset), runs_(h.channels), values_(h.channels, 0.0f) {}

bool Decoder::next() {
    if (done_ || p_ >= b_.size()) return done_ = true, false;
    Reader r{b_, p_};
    const size_t start = r.p;
    const uint32_t size = r.u32();
    const int32_t frame = int32_t(r.u32());
    if (frame == -1) {
        p_ = r.p;
        return terminated_ = done_ = true, false;
    }
    if (frame != frame_ + 1) throw FormatError("cine: frame " + std::to_string(frame) + " out of sequence");
    for (size_t c = 0; c < runs_.size(); ++c) {
        Run& run = runs_[c];
        if (run.left == 0) {
            const uint32_t v = r.u32();
            run.left = v & 0x0FFFFFFF;
            run.animated = (v & 0xF0000000) != 0;
            values_[c] = r.f32(); // first value of the run
        } else if (run.animated) {
            values_[c] = r.f32();
        }
        --run.left; // wraps for an (unobserved) zero-length run, as the exe's counter
    }
    const uint32_t subLen = r.u32();
    r.need(subLen);
    // Blobs >= 4095 bytes: the exe prints "bad subtitle data" instead (none in the retail data).
    subtitle_ = subLen < 0xFFF ? std::string_view(reinterpret_cast<const char*>(b_.data() + r.p), subLen) : std::string_view();
    r.p += subLen;
    r.skip((16 - r.p % 16) % 16);
    if (r.p - start != uint64_t(size) + 4) throw FormatError("cine: frame size mismatch at frame " + std::to_string(frame));
    p_ = r.p;
    frame_ = frame;
    return true;
}

Summary scan(std::span<const uint8_t> cine) {
    Summary s;
    s.header = parseHeader(cine);
    Decoder d(cine, s.header);
    while (d.next()) {
        locale::appendCineSubtitles(d.subtitleBlob(), d.frame(), s.subtitles);
        ++s.frames;
    }
    s.cleanEnd = !d.terminated() && d.offset() == cine.size();
    if (d.terminated()) {
        size_t p = d.offset() - 8; // the -1 frame's size field
        s.cleanEnd = true;
        for (; p + 16 <= cine.size() && s.cleanEnd; p += 16)
            s.cleanEnd = rd32(cine, p) == 12 && rd32(cine, p + 4) == 0xFFFFFFFF && rd32(cine, p + 8) == 0 && rd32(cine, p + 12) == 0;
        s.cleanEnd = s.cleanEnd && p == cine.size();
    }
    return s;
}

// ---- playback ---------------------------------------------------------------------------------------------

float CameraPose::verticalFov(float aspect) const { return 2.0f * std::atan(std::tan(horizontalFov * 0.5f) / aspect); }

Player::Player(std::vector<uint8_t> cine)
    : bytes_(std::move(cine)), header_(parseHeader(bytes_)), dec_(bytes_, header_) {
    if (!dec_.next()) throw FormatError("cine: no frames");
    cur_.assign(dec_.values().begin(), dec_.values().end());
    prev_ = cur_; // 0x453C00: both halves start at the first values
    locale::appendCineSubtitles(dec_.subtitleBlob(), 0, subs_);
}

bool Player::seek(double pos) {
    if (!fresh_) subs_.clear(); // frame 0's entries (decoded by the constructor) go out with the first seek
    fresh_ = false;
    pos_ = std::max(pos, pos_);
    const double whole = std::floor(pos_ + 1e-6); // n / 30 s * 30 may land just below n
    // Decode until the current frame is floor(pos) + 1 (0x457100), so values lerp from floor(pos).
    while (!finished_ && dec_.frame() <= int(whole)) {
        if (!dec_.next()) {
            finished_ = true;
            break;
        }
        prev_.swap(cur_);
        cur_.assign(dec_.values().begin(), dec_.values().end());
        locale::appendCineSubtitles(dec_.subtitleBlob(), dec_.frame(), subs_);
    }
    if (finished_) pos_ = std::min(pos_, double(dec_.frame()));
    frac_ = finished_ ? 1.0f : float(std::max(0.0, pos_ - whole));
    if (cutAhead()) frac_ = 0; // the next frame starts a new shot: hold the old one until then
    return !finished_;
}

bool Player::cutAhead() const {
    if (header_.cameras.empty() || finished_) return false;
    return (int(std::lround(cur_[header_.cameras[0] + 10])) & 1) != 0; // 0x453CE0, flag of camera 0's channel 10
}

float Player::value(uint32_t c) const { return prev_[c] + (cur_[c] - prev_[c]) * frac_; }

glm::quat Player::rotation(uint32_t c) const {
    glm::quat a = euler(prev_, c), b = euler(cur_, c);
    if (a.w < 0) a = -a; // 0x453F80 flips both to w >= 0 before the slerp
    if (b.w < 0) b = -b;
    return glm::slerp(a, b, frac_);
}

glm::mat4 Player::transform(uint32_t c) const { // 0x454970: scale columns of R(q), translation in row 3
    glm::mat4 m = glm::mat4_cast(rotation(c + 3));
    const glm::vec3 s = lerp3(prev_, cur_, c, frac_);
    m[0] *= s.x, m[1] *= s.y, m[2] *= s.z;
    m[3] = glm::vec4(lerp3(prev_, cur_, c + 6, frac_), 1.0f);
    return m;
}

CameraPose Player::camera() const {
    CameraPose p;
    if (header_.cameras.empty()) return p;
    // 0x455910: the first camera whose flags have bit 2, else camera 0.
    uint32_t c = header_.cameras[0];
    for (const uint32_t k : header_.cameras)
        if (int(std::lround(cur_[k + 10])) & 4) { c = k; break; }
    const glm::mat4 m = origin * transform(c);
    // World = origin * M * Rx(-90 deg): the exe camera looks along its +Y, i.e. along M's -Z, up = M's +Y.
    p.position = glm::vec3(m[3]);
    p.forward = glm::normalize(-glm::vec3(m[2]));
    p.up = glm::normalize(glm::vec3(m[1]));
    p.horizontalFov = value(c + 9);
    p.cut = (int(std::lround(cur_[c + 10])) & 1) != 0;
    return p;
}

std::vector<glm::mat4> Player::bones(const Instance& in, const scene::Skeleton& sk) const {
    const size_t n = sk.bones.size(), animated = std::min<size_t>(in.bones, n);
    std::vector<glm::mat4> world(n), out(n);
    for (size_t b = 0; b < animated; ++b) {
        const uint32_t c = in.boneChannel(uint32_t(b));
        glm::mat4 local = transform(c);
        const int32_t parent = sk.bones[b].parent;
        const int flags = int(prev_[c + kBoneFlags]); // the exe reads the frame value, not the interpolated one
        glm::mat4 parentWorld = origin;
        if (parent >= 0 && size_t(parent) < b) {
            if (flags & 2) parentWorld = world[0];
            else {
                parentWorld = world[size_t(parent)];
                if (flags & 1) { // parent-scale compensation: columns scaled by 1 / parent scale, translation kept
                    const uint32_t pc = in.boneChannel(uint32_t(parent));
                    const glm::vec3 s = lerp3(prev_, cur_, pc, frac_);
                    local[0] /= s.x, local[1] /= s.y, local[2] /= s.z;
                }
            }
        }
        world[b] = parentWorld * local;
        out[b] = world[b] * (b < in.boneFix.size() ? in.boneFix[b] : glm::mat4(1.0f));
        out[b][3] = world[b][3];
    }
    for (size_t b = animated; b < n; ++b) { // bones the stream does not animate ride on their parent (0x454BD0)
        const int32_t parent = sk.bones[b].parent;
        const glm::mat4 base = parent >= 0 && size_t(parent) < b ? out[size_t(parent)] : origin;
        out[b] = base * glm::translate(glm::mat4(1.0f), glm::vec3(sk.bones[b].pivot[0], sk.bones[b].pivot[1], sk.bones[b].pivot[2]));
    }
    return out;
}

glm::mat4 Player::originFromMarker(const Marker& m, const glm::mat4& markerIntroWorld) const {
    return markerIntroWorld * glm::inverse(transform(m.channel));
}

// ---- binding ----------------------------------------------------------------------------------------------

std::vector<std::string> unitCinematics(const cdc::Drm& unit) {
    std::vector<std::string> out;
    if (unit.sections.empty()) return out;
    const auto lv = cdc::sectionPointers(unit, 0);
    const auto data = lv.find(0x70);
    if (data == lv.end()) return out;
    const auto& d = unit.sections.at(data->second.section);
    const uint32_t base = data->second.offset;
    if (d.data.size() < base + 0x28C) return out;
    const uint32_t n = rd32(d.data, base + 0x1DC);
    const auto ptrs = cdc::sectionPointers(unit, data->second.section);
    const auto table = ptrs.find(base + 0x288);
    if (table == ptrs.end()) return out;
    const auto tp = cdc::sectionPointers(unit, table->second.section);
    for (uint32_t i = 0; i < n; ++i) {
        const auto s = tp.find(table->second.offset + 4 * i);
        if (s == tp.end()) throw FormatError("cinematic table entry " + std::to_string(i) + " not relocated");
        const auto& sd = unit.sections.at(s->second.section).data;
        if (s->second.offset >= sd.size()) throw FormatError("cinematic name out of range");
        const char* c = reinterpret_cast<const char*>(sd.data() + s->second.offset);
        out.emplace_back(c, strnlen(c, sd.size() - s->second.offset));
    }
    return out;
}

std::string introCinematic(const cdc::Drm& unit, uint32_t scriptId) {
    for (uint32_t i = 0; i < unit.sections.size(); ++i) {
        const auto& s = unit.sections[i];
        if (s.type != cdc::SectionType::Script || s.id != scriptId) continue;
        const auto t = script::parseScriptType(unit, i);
        for (const auto& m : t.members)
            if (m.typeId == kCinematicType && m.init) {
                const auto names = unitCinematics(unit);
                return *m.init < names.size() ? names[*m.init] : std::string();
            }
        return {};
    }
    return {};
}

} // namespace ou::cine
