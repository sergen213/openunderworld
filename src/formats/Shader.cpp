// D3D9 SM2/SM3 bytecode parser + disassembler and the TRU ShaderLib container (docs/formats/SHADERS.md).
#include "formats/Shader.hpp"

#include "cdc/Cdc.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>

namespace ou::formats::d3d9 {
namespace {

using cdc::FormatError;

const char* const kOps[] = {
    "nop", "mov", "add", "sub", "mad", "mul", "rcp", "rsq", "dp3", "dp4", "min", "max", "slt", "sge", "exp", "log",
    "lit", "dst", "lrp", "frc", "m4x4", "m4x3", "m3x4", "m3x3", "m3x2", "call", "callnz", "loop", "ret", "endloop",
    "label", "dcl", "pow", "crs", "sgn", "abs", "nrm", "sincos", "rep", "endrep", "if", "ifc", "else", "endif",
    "break", "breakc", "mova", "defb", "defi"};
const char* const kTexOps[] = {
    "texcoord", "texkill", "texld", "texbem", "texbeml", "texreg2ar", "texreg2gb", "texm3x2pad", "texm3x2tex",
    "texm3x3pad", "texm3x3tex", "reserved0", "texm3x3spec", "texm3x3vspec", "expp", "logp", "cnd", "def",
    "texreg2rgb", "texdp3tex", "texm3x2depth", "texdp3", "texm3x3", "texdepth", "cmp", "bem", "dp2add", "dsx", "dsy",
    "texldd", "setp", "texldl", "breakp"};
const char* const kUsage[] = {"position", "blendweight", "blendindices", "normal", "psize", "texcoord", "tangent",
                              "binormal", "tessfactor", "positiont", "color", "fog", "depth", "sample"};
const char* const kCmp[] = {"", "_gt", "_eq", "_ge", "_lt", "_ne", "_le", ""};

// Ops whose first parameter is not a destination.
bool noDst(uint16_t op) {
    switch (op) {
    case Nop: case Call: case CallNZ: case Loop: case Ret: case EndLoop: case Label: case Rep: case EndRep: case If:
    case IfC: case Else: case EndIf: case Break: case BreakC: case BreakP: case Phase:
        return true;
    default: return false;
    }
}

Reg regType(uint32_t t) { return Reg(((t >> 28) & 7) | (((t >> 11) & 3) << 3)); }

struct Cursor {
    std::span<const uint32_t> t;
    size_t p = 0;
    uint32_t next() {
        if (p >= t.size()) throw FormatError("shader token stream truncated");
        return t[p++];
    }
};

Src readSrc(Cursor& c, uint32_t tok, bool sm2) {
    Src s;
    s.type = regType(tok);
    s.num = uint16_t(tok & 0x7FF);
    s.swizzle = uint8_t(tok >> 16);
    s.mod = uint8_t((tok >> 24) & 15);
    if (tok & (1u << 13)) {
        s.relative = true;
        if (sm2) {
            const uint32_t r = c.next();
            s.relType = regType(r);
            s.relNum = uint16_t(r & 0x7FF);
            s.relComp = uint8_t((r >> 16) & 3);
        }
    }
    return s;
}

Dst readDst(Cursor& c, uint32_t tok, bool sm2) {
    Dst d;
    d.type = regType(tok);
    d.num = uint16_t(tok & 0x7FF);
    d.mask = uint8_t((tok >> 16) & 15);
    d.mod = uint8_t((tok >> 20) & 15);
    d.shift = int8_t(int8_t(uint8_t((tok >> 24) & 15) << 4) >> 4);
    if ((tok & (1u << 13)) && sm2) c.next(); // relative output addressing (vs_3_0 o[aL]); not used by TRU [O]
    return d;
}

std::string regName(Reg type, uint16_t num, bool pixel) {
    char b[32];
    switch (type) {
    case Reg::Temp: std::snprintf(b, sizeof b, "r%u", num); break;
    case Reg::Input: std::snprintf(b, sizeof b, "v%u", num); break;
    case Reg::Const: std::snprintf(b, sizeof b, "c%u", num); break;
    case Reg::Const2: std::snprintf(b, sizeof b, "c%u", num + 2048); break;
    case Reg::Const3: std::snprintf(b, sizeof b, "c%u", num + 4096); break;
    case Reg::Const4: std::snprintf(b, sizeof b, "c%u", num + 6144); break;
    case Reg::Addr: std::snprintf(b, sizeof b, pixel ? "t%u" : "a%u", num); break;
    case Reg::RastOut: std::snprintf(b, sizeof b, "%s", num == 0 ? "oPos" : num == 1 ? "oFog" : "oPts"); break;
    case Reg::AttrOut: std::snprintf(b, sizeof b, "oD%u", num); break;
    case Reg::Output: std::snprintf(b, sizeof b, "o%u", num); break;
    case Reg::ConstInt: std::snprintf(b, sizeof b, "i%u", num); break;
    case Reg::ColorOut: std::snprintf(b, sizeof b, "oC%u", num); break;
    case Reg::DepthOut: std::snprintf(b, sizeof b, "oDepth"); break;
    case Reg::Sampler: std::snprintf(b, sizeof b, "s%u", num); break;
    case Reg::ConstBool: std::snprintf(b, sizeof b, "b%u", num); break;
    case Reg::Loop: std::snprintf(b, sizeof b, "aL"); break;
    case Reg::TempFloat16: std::snprintf(b, sizeof b, "half%u", num); break;
    case Reg::Misc: std::snprintf(b, sizeof b, "%s", num == 0 ? "vPos" : num == 1 ? "vFace" : "misc?"); break;
    case Reg::Label: std::snprintf(b, sizeof b, "l%u", num); break;
    case Reg::Predicate: std::snprintf(b, sizeof b, "p%u", num); break;
    }
    return b;
}

bool g_pixel = false; // formatting context of disassemble() (single-threaded tool use)

} // namespace

const char* opName(uint16_t op) {
    static thread_local char buf[16];
    if (op < std::size(kOps)) return kOps[op];
    if (op >= TexCoord && op < TexCoord + std::size(kTexOps)) return kTexOps[op - TexCoord];
    if (op == Phase) return "phase";
    if (op == Comment) return "comment";
    if (op == End) return "end";
    std::snprintf(buf, sizeof buf, "op%u", op);
    return buf;
}

bool opSupported(uint16_t op) {
    switch (op) {
    case Nop: case Mov: case Add: case Sub: case Mad: case Mul: case Rcp: case Rsq: case Dp3: case Dp4: case Min:
    case Max: case Slt: case Sge: case Exp: case Log: case Lit: case DstOp: case Lrp: case Frc: case M4x4: case M4x3:
    case M3x4: case M3x3: case M3x2: case Loop: case EndLoop: case Dcl: case Pow: case Crs: case Sgn: case Abs:
    case Nrm: case SinCos: case Rep: case EndRep: case If: case IfC: case Else: case EndIf: case Break: case BreakC:
    case MovA: case DefB: case DefI: case TexKill: case Tex: case Def: case Cmp: case Dp2Add: case Dsx: case Dsy:
    case TexLdd: case SetP: case TexLdl: case BreakP: case ExpP: case LogP:
        return true;
    default: return false;
    }
}

const Constant* Program::constant(std::string_view name) const {
    for (const auto& c : constants)
        if (c.name == name) return &c;
    return nullptr;
}

Program parse(std::span<const uint32_t> t) {
    Program p;
    if (t.empty()) throw FormatError("empty shader");
    const uint32_t ver = t[0];
    if ((ver >> 16) != 0xFFFE && (ver >> 16) != 0xFFFF) throw FormatError("not a D3D9 shader version token");
    p.pixel = (ver >> 16) == 0xFFFF;
    p.major = uint8_t(ver >> 8), p.minor = uint8_t(ver);
    if (p.major < 2) throw FormatError("shader model 1 is not supported");
    Cursor c{t, 1};
    for (;;) {
        const uint32_t tok = c.next();
        const uint16_t op = uint16_t(tok);
        if (op == End) break;
        if (op == Comment) {
            const uint32_t n = (tok >> 16) & 0x7FFF;
            if (c.p + n > t.size()) throw FormatError("shader comment past end");
            const auto body = t.subspan(c.p, n);
            c.p += n;
            if (n >= 7 && body[0] == 0x42415443) { // "CTAB"
                const auto* b = reinterpret_cast<const uint8_t*>(body.data()) + 4;
                const size_t size = 4 * size_t(n) - 4;
                auto u32 = [&](size_t o) {
                    if (o + 4 > size) throw FormatError("CTAB truncated");
                    uint32_t v;
                    std::memcpy(&v, b + o, 4);
                    return v;
                };
                auto u16 = [&](size_t o) { return uint16_t(u32(o & ~size_t(1)) >> (o & 1 ? 16 : 0)); };
                auto str = [&](size_t o) {
                    std::string s;
                    while (o < size && b[o]) s += char(b[o++]);
                    return s;
                };
                p.creator = str(u32(4));
                const uint32_t count = u32(12), info = u32(16);
                for (uint32_t i = 0; i < count; ++i) {
                    const size_t e = info + 20 * size_t(i);
                    Constant k;
                    k.name = str(u32(e));
                    k.registerSet = u16(e + 4);
                    k.registerIndex = u16(e + 6);
                    k.registerCount = u16(e + 8);
                    p.constants.push_back(std::move(k));
                }
            }
            continue;
        }
        const uint32_t len = (tok >> 24) & 15;
        if (c.p + len > t.size()) throw FormatError("instruction past end of shader");
        Cursor ic{t.subspan(0, c.p + len), c.p};
        c.p += len;
        Instruction in;
        in.op = op;
        in.control = uint8_t((tok >> 16) & 0xFF);
        in.predicated = tok & (1u << 28);
        if (op == Dcl) {
            const uint32_t u = ic.next();
            in.hasDst = true;
            in.dst = readDst(ic, ic.next(), true);
            in.usage = Usage(u & 31);
            in.usageIndex = uint8_t((u >> 16) & 15);
            in.textureType = uint8_t((u >> 27) & 15);
        } else if (op == Def || op == DefI || op == DefB) {
            in.hasDst = true;
            in.dst = readDst(ic, ic.next(), true);
            for (int k = 0; k < (op == DefB ? 1 : 4); ++k) in.value[k] = ic.next();
        } else {
            if (!noDst(op) && ic.p < ic.t.size()) {
                in.hasDst = true;
                in.dst = readDst(ic, ic.next(), true);
            }
            if (in.predicated) in.pred = readSrc(ic, ic.next(), true);
            while (ic.p < ic.t.size()) in.src.push_back(readSrc(ic, ic.next(), true));
        }
        if (ic.p != ic.t.size()) throw FormatError(std::string("bad operand count for ") + opName(op));
        p.instructions.push_back(std::move(in));
    }
    p.tokenCount = c.p;
    return p;
}

Program parse(std::span<const uint8_t> bytes) {
    if (bytes.size() % 4) throw FormatError("shader size not a multiple of 4");
    std::vector<uint32_t> t(bytes.size() / 4);
    std::memcpy(t.data(), bytes.data(), bytes.size());
    return parse(std::span<const uint32_t>(t));
}

std::string format(const Src& s) {
    std::string r = regName(s.type, s.num, g_pixel);
    if (s.relative) {
        r = regName(s.type, 0, g_pixel);
        r.resize(r.size() - 1);
        r += "[" + regName(s.relType, s.relNum, g_pixel) + "." + "xyzw"[s.relComp] + " + " + std::to_string(s.num) + "]";
    }
    if (s.swizzle != 0xE4) {
        r += '.';
        for (int i = 0; i < 4; ++i) r += "xyzw"[(s.swizzle >> 2 * i) & 3];
    }
    switch (s.mod) {
    case 0: return r;
    case 1: return "-" + r;
    case 2: return r + "_bias";
    case 3: return "-" + r + "_bias";
    case 4: return r + "_bx2";
    case 5: return "-" + r + "_bx2";
    case 6: return "1-" + r;
    case 7: return r + "_x2";
    case 8: return "-" + r + "_x2";
    case 9: return r + "_dz";
    case 10: return r + "_dw";
    case 11: return r + "_abs";
    case 12: return "-" + r + "_abs";
    case 13: return "!" + r;
    default: return r + "_mod" + std::to_string(s.mod);
    }
}

std::string format(const Dst& d) {
    std::string r = regName(d.type, d.num, g_pixel);
    if (d.mask != 15) {
        r += '.';
        for (int i = 0; i < 4; ++i)
            if (d.mask >> i & 1) r += "xyzw"[i];
    }
    return r;
}

std::string disassemble(const Program& p) {
    g_pixel = p.pixel;
    std::string out = (p.pixel ? "ps_" : "vs_") + std::to_string(p.major) + "_" + std::to_string(p.minor) + "\n";
    for (const auto& k : p.constants)
        out += "// " + k.name + " " + "bics"[k.registerSet & 3] + std::to_string(k.registerIndex) +
               (k.registerCount > 1 ? " [" + std::to_string(k.registerCount) + "]" : "") + "\n";
    int indent = 0;
    for (const auto& in : p.instructions) {
        if (in.op == EndIf || in.op == EndLoop || in.op == EndRep || in.op == Else) --indent;
        out += std::string(2 * std::max(indent, 0), ' ');
        if (in.predicated) out += "(" + format(in.pred) + ") ";
        if (in.op == Dcl) {
            std::string what;
            if (in.dst.type == Reg::Sampler)
                what = in.textureType == 3 ? "_cube" : in.textureType == 4 ? "_volume" : "_2d";
            else if (p.major >= 3 || !p.pixel)
                what = std::string("_") + (size_t(in.usage) < std::size(kUsage) ? kUsage[size_t(in.usage)] : "?") +
                       std::to_string(in.usageIndex);
            out += "dcl" + what + " " + format(in.dst);
            if (in.dst.mod & 4) out += " (centroid)";
        } else if (in.op == Def) {
            char b[160];
            float f[4];
            std::memcpy(f, in.value, 16);
            std::snprintf(b, sizeof b, "def %s, %.9g, %.9g, %.9g, %.9g", format(in.dst).c_str(), f[0], f[1], f[2], f[3]);
            out += b;
        } else if (in.op == DefI) {
            out += "defi " + format(in.dst);
            for (int k = 0; k < 4; ++k) out += ", " + std::to_string(int32_t(in.value[k]));
        } else if (in.op == DefB) {
            out += "defb " + format(in.dst) + (in.value[0] ? ", true" : ", false");
        } else {
            out += opName(in.op);
            if (in.op == IfC || in.op == BreakC || in.op == SetP) out += kCmp[in.control & 7];
            if (in.op == Tex && in.control == 1) out += "p";
            if (in.op == Tex && in.control == 2) out += "b";
            if (in.hasDst) {
                if (in.dst.mod & 1) out += "_sat";
                if (in.dst.mod & 2) out += "_pp";
                if (in.dst.mod & 4) out += "_centroid";
            }
            bool first = true;
            auto sep = [&] { out += first ? " " : ", "; first = false; };
            if (in.hasDst) sep(), out += format(in.dst);
            for (const auto& s : in.src) sep(), out += format(s);
        }
        out += "\n";
        if (in.op == If || in.op == IfC || in.op == Loop || in.op == Rep || in.op == Else) ++indent;
    }
    return out;
}

const ShaderLibEntry* ShaderLib::find(uint32_t index) const {
    for (const auto& e : entries)
        if (e.index == index) return &e;
    return nullptr;
}

ShaderLib parseShaderLib(std::span<const uint8_t> d) {
    auto u32 = [&](size_t o) {
        if (o > d.size() || d.size() - o < 4) throw FormatError("shader lib truncated");
        uint32_t v;
        std::memcpy(&v, d.data() + o, 4);
        return v;
    };
    ShaderLib lib;
    const uint32_t kind = u32(0);
    if (kind > 1) throw FormatError("unknown shader lib kind " + std::to_string(kind));
    lib.pixel = kind == 1;
    const uint32_t header = u32(4);
    lib.slots = header / 4;
    for (uint32_t i = 0; i < lib.slots; ++i) {
        const uint32_t off = u32(12 + 4 * size_t(i));
        if (off == ~0u) continue;
        const size_t e = 4 + size_t(off);
        const uint32_t size = u32(e + 4) & 0xFFFF, flags = u32(e + 4) >> 16;
        // size counts the 16-byte entry header [O: entries are packed back to back]
        if (size < 16 || e > d.size() || d.size() - e < size) throw FormatError("shader lib entry past section end");
        ShaderLibEntry en;
        en.index = i;
        en.libHash = u32(e);
        en.flags = uint16_t(flags);
        en.hash = u32(e + 8) | uint64_t(u32(e + 12)) << 32;
        en.bytecode = d.subspan(e + 16, size - 16);
        lib.entries.push_back(en);
    }
    return lib;
}

InstallShaders collectInstallShaders(const std::string& gameDir) {
    namespace fs = std::filesystem;
    InstallShaders out;
    std::map<std::pair<bool, uint64_t>, size_t> seen;
    std::vector<fs::path> archives;
    if (fs::exists(fs::path(gameDir) / "patch.000")) archives.push_back(fs::path(gameDir) / "patch.000");
    archives.push_back(fs::path(gameDir) / "bigfile.000");
    for (const auto& a : archives) {
        const cdc::Bigfile bf(a);
        for (const auto& rec : bf.records()) {
            if (rec.size < 24 || rec.size > (256u << 20)) continue;
            auto bytes = bf.read(rec);
            uint32_t magic;
            std::memcpy(&magic, bytes.data(), 4);
            if (!cdc::isCdrm(bytes) && magic != cdc::kDrmVersion) continue;
            cdc::Drm drm;
            try {
                drm = cdc::parseDrm(std::move(bytes));
            } catch (const FormatError&) {
                continue; // not a DRM after all (e.g. data that starts with 19)
            }
            ++out.drms;
            for (const auto& s : drm.sections) {
                if (s.type != cdc::SectionType::ShaderLib) continue;
                ++out.libs;
                try {
                    const ShaderLib lib = parseShaderLib(s.data);
                    for (const auto& e : lib.entries) {
                        ++out.entries;
                        const auto [it, fresh] = seen.try_emplace({lib.pixel, e.hash}, out.shaders.size());
                        if (!fresh) {
                            const auto& old = out.shaders[it->second].bytecode;
                            if (!std::equal(old.begin(), old.end(), e.bytecode.begin(), e.bytecode.end())) ++out.hashMismatches;
                            continue;
                        }
                        out.shaders.push_back({lib.pixel, e.hash, {e.bytecode.begin(), e.bytecode.end()}, s.id, e.index});
                    }
                } catch (const FormatError& ex) {
                    out.errors.push_back("shader lib " + std::to_string(s.id) + ": " + ex.what());
                }
            }
        }
    }
    return out;
}

} // namespace ou::formats::d3d9
