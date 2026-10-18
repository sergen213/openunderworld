// D3D9 SM3 -> GLSL 4.50 translator (docs/formats/SHADERS.md "Translation"). Straight-line translation: every
// D3D register becomes a vec4 local, every instruction one masked assignment; flow control maps 1:1.
#include "render/ShaderTranslate.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>

#include <shaderc/shaderc.h>

namespace ou::render::sm3 {
namespace {

using namespace formats::d3d9;

struct Fail : std::runtime_error { using std::runtime_error::runtime_error; };

const char* kPrelude = R"(#version 450
#ifdef OU_VULKAN
#define OU_UBO(s, b, gl) layout(std140, set = s, binding = b)
#define OU_SAMPLER(n) layout(set = 1, binding = 1 + n)
#define OU_DFDY_SIGN 1.0
#else
#define OU_UBO(s, b, gl) layout(std140, binding = gl)
#define OU_SAMPLER(n) layout(binding = n)
#define OU_DFDY_SIGN -1.0
#endif
OU_UBO(0, 0, 0) uniform OuFrame { vec4 vs[256]; vec4 ps[224]; ivec4 vsI[16]; ivec4 psI[16]; uvec4 bools; vec4 screen; } ouFrame;
OU_UBO(1, 0, 1) uniform OuMaterial { vec4 vs[40]; vec4 ps[40]; vec4 fixedParams; } ouMat;
#ifdef OU_VULKAN
layout(push_constant) uniform OuObject { vec4 v[8]; } ouObj;
#else
layout(std140, binding = 2) uniform OuObject { vec4 v[8]; } ouObj;
#endif
)";

std::string flt(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    char b[48];
    if (!std::isfinite(f)) {
        std::snprintf(b, sizeof b, "uintBitsToFloat(0x%08Xu)", bits);
        return b;
    }
    std::snprintf(b, sizeof b, "%.9g", double(f));
    std::string s = b;
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
    return s;
}

int varyingLocation(Usage u, int index) {
    switch (u) {
    case Usage::Texcoord: return index;
    case Usage::Color: return 8 + index;
    case Usage::Fog: return 10;
    default: return -1;
    }
}

int attributeLocation(Usage u, int index) {
    if (index != 0 && u != Usage::Texcoord) return -1;
    switch (u) {
    case Usage::Position: return kAttrPosition;
    case Usage::Normal: return kAttrNormal;
    case Usage::Texcoord: return index == 0 ? kAttrTexcoord0 : index == 1 ? kAttrTexcoord1 : -1;
    case Usage::Color: return kAttrColor0;
    case Usage::Tangent: return kAttrTangent;
    case Usage::Binormal: return kAttrBinormal;
    default: return -1;
    }
}

class Translator {
public:
    Translator(const Program& p, Glsl& out) : p_(p), out_(out) {}

    std::string run() {
        // Pass 1: declarations and register usage.
        for (const auto& in : p_.instructions) {
            if (in.op == Dcl) {
                if (in.dst.type == Reg::Sampler) {
                    out_.samplers |= uint16_t(1u << (in.dst.num & 15));
                    out_.samplerType[in.dst.num & 15] = in.textureType ? in.textureType : 2;
                } else if (in.dst.type == Reg::Input) inputs_[in.dst.num] = &in;
                else if (in.dst.type == Reg::Output) outputs_[in.dst.num] = &in;
                else if (in.dst.type == Reg::Misc) misc_.insert(in.dst.num);
                continue;
            }
            if (in.op == Def) floatDefs_[in.dst.num] = &in;
            else if (in.op == DefI) intDefs_[in.dst.num] = &in;
            else if (in.op == DefB) boolDefs_[in.dst.num] = &in;
            auto note = [&](Reg t, unsigned n) {
                if (t == Reg::Temp) temps_ = std::max(temps_, n + 1);
                if (t == Reg::Addr && !p_.pixel) usesA0_ = true;
                if (t == Reg::Predicate) usesP0_ = true;
                if (t == Reg::Loop) usesAL_ = true;
                if (t == Reg::ColorOut) colorOuts_ = std::max(colorOuts_, n + 1);
                if (t == Reg::DepthOut) depthOut_ = true;
                if (t == Reg::Output) outputs_.try_emplace(n, nullptr);
            };
            if (in.hasDst) note(in.dst.type, in.dst.num);
            for (const auto& s : in.src) {
                note(s.type, s.num);
                if (s.relative) note(s.relType, s.relNum);
            }
            if (in.op == Loop) usesAL_ = true;
        }
        std::string h = kPrelude;
        // Samplers.
        for (int n = 0; n < 16; ++n)
            if (out_.samplers >> n & 1) {
                const char* t = out_.samplerType[n] == 3 ? "samplerCube" : out_.samplerType[n] == 4 ? "sampler3D" : "sampler2D";
                h += "OU_SAMPLER(" + std::to_string(n) + ") uniform " + t + " s" + std::to_string(n) + ";\n";
            }
        // Stage interface.
        std::string pre, post;
        if (!p_.pixel) {
            for (const auto& [n, d] : inputs_) {
                const int loc = attributeLocation(d->usage, d->usageIndex);
                if (loc >= 0) {
                    h += "layout(location = " + std::to_string(loc) + ") in vec4 ouAttr" + std::to_string(loc) + ";\n";
                    pre += "    vec4 v" + std::to_string(n) + " = ouAttr" + std::to_string(loc) + ";\n";
                    out_.attributes |= uint8_t(1u << loc);
                } else {
                    pre += "    vec4 v" + std::to_string(n) + " = vec4(0.0, 0.0, 0.0, 1.0); // unsupplied input\n";
                }
            }
            for (const auto& [n, d] : outputs_) {
                pre += "    vec4 o" + std::to_string(n) + " = vec4(0.0);\n";
                if (!d) continue;
                if (d->usage == Usage::Position && d->usageIndex == 0) {
                    // D3D clip z is [0, w]; GL wants [-w, w]. Vulkan keeps D3D depth (negative-height viewport flips y).
                    post += "    gl_Position = o" + std::to_string(n) + ";\n#ifndef OU_VULKAN\n    gl_Position.z = 2.0 * gl_Position.z - gl_Position.w;\n#endif\n";
                } else if (d->usage == Usage::PSize) {
                    // point size: unused (no point sprites in the draws we translate)
                } else {
                    const int loc = varyingLocation(d->usage, d->usageIndex);
                    if (loc < 0) throw Fail("unsupported VS output usage " + std::to_string(int(d->usage)));
                    h += "layout(location = " + std::to_string(loc) + ") out vec4 ouVar" + std::to_string(loc) + ";\n";
                    post += "    ouVar" + std::to_string(loc) + " = o" + std::to_string(n) + ";\n";
                }
            }
        } else {
            for (const auto& [n, d] : inputs_) {
                const int loc = varyingLocation(d->usage, d->usageIndex);
                if (loc < 0) throw Fail("unsupported PS input usage " + std::to_string(int(d->usage)));
                h += "layout(location = " + std::to_string(loc) + ") " + ((d->dst.mod & 4) ? "centroid " : "") + "in vec4 ouVar" +
                     std::to_string(loc) + ";\n";
                pre += "    vec4 v" + std::to_string(n) + " = ouVar" + std::to_string(loc) + ";\n";
            }
            if (misc_.count(0))
                pre += "    vec4 vPos = vec4(gl_FragCoord.x - 0.5, "
                       "(OU_DFDY_SIGN > 0.0 ? gl_FragCoord.y : ouFrame.screen.y - gl_FragCoord.y) - 0.5, 0.0, 0.0);\n";
            if (misc_.count(1)) pre += "    vec4 vFace = vec4(gl_FrontFacing ? 1.0 : -1.0);\n";
            h += "layout(location = 0) out vec4 ouColor;\n";
            for (unsigned n = 0; n < std::max(colorOuts_, 1u); ++n) pre += "    vec4 oC" + std::to_string(n) + " = vec4(0.0);\n";
            if (depthOut_) pre += "    vec4 oDepth = vec4(gl_FragCoord.z);\n";
            post += "    ouColor = oC0;\n"
                    "    if (ouMat.fixedParams.z >= 0.0 && ouColor.a < ouMat.fixedParams.z) discard; // D3DRS_ALPHAREF test\n";
            if (depthOut_) post += "    gl_FragDepth = oDepth.x;\n";
        }
        for (unsigned n = 0; n < temps_; ++n) pre += "    vec4 r" + std::to_string(n) + " = vec4(0.0);\n";
        if (usesA0_) pre += "    ivec4 a0 = ivec4(0);\n";
        if (usesAL_) pre += "    int aL = 0;\n";
        if (usesP0_) pre += "    bvec4 p0 = bvec4(false);\n";
        for (const auto& [n, d] : floatDefs_)
            pre += "    const vec4 c" + std::to_string(n) + "_def = vec4(" + flt(d->value[0]) + ", " + flt(d->value[1]) + ", " +
                   flt(d->value[2]) + ", " + flt(d->value[3]) + ");\n";
        for (const auto& [n, d] : intDefs_)
            pre += "    const ivec4 i" + std::to_string(n) + "_def = ivec4(" + std::to_string(int32_t(d->value[0])) + ", " +
                   std::to_string(int32_t(d->value[1])) + ", " + std::to_string(int32_t(d->value[2])) + ", " +
                   std::to_string(int32_t(d->value[3])) + ");\n";

        std::string body;
        int depth = 1, loops = 0;
        for (const auto& in : p_.instructions) {
            if (in.op == Dcl || in.op == Def || in.op == DefI || in.op == DefB || in.op == Nop) continue;
            if (in.op == EndIf || in.op == EndLoop || in.op == EndRep || in.op == Else) --depth;
            const std::string ind(4 * size_t(std::max(depth, 1)), ' ');
            std::string line = statement(in, loops);
            if (in.predicated && in.hasDst) line = "if (" + predicate(in.pred) + ") { " + line + " }";
            body += ind + line + "\n";
            if (in.op == If || in.op == IfC || in.op == Loop || in.op == Rep || in.op == Else) ++depth;
        }
        return h + "void main() {\n" + pre + body + post + "}\n";
    }

private:
    const Program& p_;
    Glsl& out_;
    std::map<unsigned, const Instruction*> inputs_, outputs_, floatDefs_, intDefs_, boolDefs_;
    std::set<unsigned> misc_;
    unsigned temps_ = 0, colorOuts_ = 0;
    bool usesA0_ = false, usesAL_ = false, usesP0_ = false, depthOut_ = false;

    std::string constRef(unsigned n) const {
        if (floatDefs_.count(n)) return "c" + std::to_string(n) + "_def";
        if (!p_.pixel) {
            if (n < unsigned(kObjectVs)) return "ouObj.v[" + std::to_string(n) + "]";
            if (n >= unsigned(kMaterialVsBase) && n < unsigned(kMaterialVsBase + kMaterialVs))
                return "ouMat.vs[" + std::to_string(n - kMaterialVsBase) + "]";
            if (n >= unsigned(kFrameVs)) throw Fail("VS constant out of range");
            // fogScaleAndOffset: uploaded only for fogged materials (flags 0x04), else 0 (tru.exe 0x5068E6).
            if (n == 22) return "((int(ouMat.fixedParams.w) & 1) != 0 ? ouFrame.vs[22] : vec4(0.0))";
            return "ouFrame.vs[" + std::to_string(n) + "]";
        }
        if (n == 2) return "vec4(ouMat.fixedParams.x)";
        if (n == 98) return "vec4(ouMat.fixedParams.y)";
        if (n >= unsigned(kMaterialPsBase) && n < unsigned(kMaterialPsBase + kMaterialPs))
            return "ouMat.ps[" + std::to_string(n - kMaterialPsBase) + "]";
        if (n >= unsigned(kFramePs)) throw Fail("PS constant out of range");
        // FogColor: 0 for the additive blend modes 5/6 (tru.exe 0x5066A0).
        if (n == 0) return "((int(ouMat.fixedParams.w) & 2) != 0 ? vec4(0.0) : ouFrame.ps[0])";
        return "ouFrame.ps[" + std::to_string(n) + "]";
    }

    std::string relIndex(const Src& s) const {
        std::string idx;
        if (s.relType == Reg::Addr) idx = std::string("a0.") + "xyzw"[s.relComp];
        else if (s.relType == Reg::Loop) idx = "aL";
        else throw Fail("unsupported relative address register");
        return idx + " + " + std::to_string(s.num);
    }

    // Unswizzled, unmodified vec4 value of a source register.
    std::string regValue(const Src& s) const {
        const std::string n = std::to_string(s.num);
        switch (s.type) {
        case Reg::Temp: return "r" + n;
        case Reg::Input: return "v" + n;
        case Reg::Const:
            if (s.relative) {
                // Relative addressing reads the frame register file (TRU: SkinMatrices c85+ only, [O] survey).
                const int size = p_.pixel ? kFramePs : kFrameVs;
                return std::string("ouFrame.") + (p_.pixel ? "ps" : "vs") + "[clamp(" + relIndex(s) + ", 0, " +
                       std::to_string(size - 1) + ")]";
            }
            return constRef(s.num);
        case Reg::ConstInt:
            if (intDefs_.count(s.num)) return "vec4(i" + n + "_def)";
            return std::string("vec4(ouFrame.") + (p_.pixel ? "psI[" : "vsI[") + n + "])";
        case Reg::ConstBool: return "vec4(" + boolExpr(s.num) + " ? 1.0 : 0.0)";
        case Reg::Addr:
            if (p_.pixel) throw Fail("ps texture registers (SM1/2) are not supported");
            return "vec4(a0)";
        case Reg::Loop: return "vec4(float(aL))";
        case Reg::Misc: return s.num == 0 ? "vPos" : "vFace";
        case Reg::Predicate: return "vec4(p0)";
        case Reg::Output: return "o" + n;
        case Reg::ColorOut: return "oC" + n;
        default: throw Fail("unsupported source register type " + std::to_string(int(s.type)));
        }
    }

    std::string boolExpr(unsigned n) const {
        if (const auto it = boolDefs_.find(n); it != boolDefs_.end()) return it->second->value[0] ? "true" : "false";
        return std::string("((ouFrame.bools.") + (p_.pixel ? "y" : "x") + " >> " + std::to_string(n) + "u) & 1u) != 0u";
    }

    static std::string swz(uint8_t sw) {
        std::string s = ".";
        for (int i = 0; i < 4; ++i) s += "xyzw"[(sw >> 2 * i) & 3];
        return s;
    }

    std::string src(const Src& s) const {
        std::string v = regValue(s);
        if (s.swizzle != 0xE4) v = "(" + v + ")" + swz(s.swizzle);
        switch (s.mod) {
        case 0: return v;
        case 1: return "(-" + v + ")";
        case 2: return "(" + v + " - 0.5)";
        case 3: return "(0.5 - " + v + ")";
        case 4: return "(2.0 * " + v + " - 1.0)";
        case 5: return "(1.0 - 2.0 * " + v + ")";
        case 6: return "(1.0 - " + v + ")";
        case 7: return "(2.0 * " + v + ")";
        case 8: return "(-2.0 * " + v + ")";
        case 11: return "abs(" + v + ")";
        case 12: return "(-abs(" + v + "))";
        default: throw Fail("unsupported source modifier " + std::to_string(s.mod));
        }
    }
    std::string scalar(const Src& s) const { return src(s) + ".x"; } // replicate swizzle: component 0

    std::string dstName(const Dst& d) const {
        const std::string n = std::to_string(d.num);
        switch (d.type) {
        case Reg::Temp: return "r" + n;
        case Reg::Output: return "o" + n;
        case Reg::ColorOut: return "oC" + n;
        case Reg::DepthOut: return "oDepth";
        case Reg::Addr: return "a0";
        case Reg::Predicate: return "p0";
        default: throw Fail("unsupported destination register type " + std::to_string(int(d.type)));
        }
    }
    static std::string mask(uint8_t m) {
        std::string s = ".";
        for (int i = 0; i < 4; ++i)
            if (m >> i & 1) s += "xyzw"[i];
        return s;
    }

    // dst.mask = expr(vec4).mask with saturate.
    std::string assign(const Instruction& in, const std::string& vec4expr) const {
        if (in.dst.shift) throw Fail("result shift (SM1) not supported");
        std::string e = vec4expr;
        if (in.dst.mod & 1) e = "clamp(" + e + ", 0.0, 1.0)";
        const std::string m = mask(in.dst.mask);
        return dstName(in.dst) + m + " = (" + e + ")" + m + ";";
    }

    static const char* cmpOp(uint8_t c) {
        switch (c & 7) {
        case 1: return ">";
        case 2: return "==";
        case 3: return ">=";
        case 4: return "<";
        case 5: return "!=";
        case 6: return "<=";
        default: throw Fail("bad comparison");
        }
    }
    static const char* cmpFn(uint8_t c) {
        switch (c & 7) {
        case 1: return "greaterThan";
        case 2: return "equal";
        case 3: return "greaterThanEqual";
        case 4: return "lessThan";
        case 5: return "notEqual";
        case 6: return "lessThanEqual";
        default: throw Fail("bad comparison");
        }
    }

    std::string predicate(const Src& s) const {
        if (s.type != Reg::Predicate) throw Fail("predicate is not p0");
        const std::string c = std::string("p0.") + "xyzw"[s.swizzle & 3];
        return s.mod == 13 ? "!" + c : c;
    }

    std::string matrix(const Instruction& in, int rows, int cols) const {
        const char* dotv = cols == 4 ? "" : ".xyz";
        std::string comps[4] = {"0.0", "0.0", "0.0", "0.0"};
        for (int r = 0; r < rows; ++r) {
            Src row = in.src[1];
            row.num = uint16_t(row.num + r);
            comps[r] = "dot((" + src(in.src[0]) + ")" + dotv + ", (" + src(row) + ")" + dotv + ")";
        }
        return assign(in, "vec4(" + comps[0] + ", " + comps[1] + ", " + comps[2] + ", " + comps[3] + ")");
    }

    std::string sampler(const Src& s) const {
        if (s.type != Reg::Sampler) throw Fail("texture op without sampler");
        return "s" + std::to_string(s.num);
    }
    std::string coords(const Src& c, const Src& s) const {
        const uint8_t t = out_.samplerType[s.num & 15];
        return "(" + src(c) + ")" + (t == 3 || t == 4 ? ".xyz" : ".xy");
    }
    std::string texResult(const Instruction& in, const std::string& fetch) const {
        const Src& s = in.src[1];
        std::string e = fetch;
        if (s.swizzle != 0xE4) e = "(" + e + ")" + swz(s.swizzle);
        return assign(in, e);
    }

    std::string statement(const Instruction& in, int& loops) {
        const auto& s = in.src;
        auto need = [&](size_t n) {
            if (s.size() < n) throw Fail(std::string("missing operands for ") + opName(in.op));
        };
        switch (in.op) {
        case Mov: need(1); return assign(in, src(s[0]));
        case Add: need(2); return assign(in, src(s[0]) + " + " + src(s[1]));
        case Sub: need(2); return assign(in, src(s[0]) + " - " + src(s[1]));
        case Mul: need(2); return assign(in, src(s[0]) + " * " + src(s[1]));
        case Mad: need(3); return assign(in, src(s[0]) + " * " + src(s[1]) + " + " + src(s[2]));
        case Rcp: need(1); return assign(in, "vec4(1.0 / " + scalar(s[0]) + ")");
        case Rsq: need(1); return assign(in, "vec4(inversesqrt(abs(" + scalar(s[0]) + ")))");
        case Dp3: need(2); return assign(in, "vec4(dot((" + src(s[0]) + ").xyz, (" + src(s[1]) + ").xyz))");
        case Dp4: need(2); return assign(in, "vec4(dot(" + src(s[0]) + ", " + src(s[1]) + "))");
        case Min: need(2); return assign(in, "min(" + src(s[0]) + ", " + src(s[1]) + ")");
        case Max: need(2); return assign(in, "max(" + src(s[0]) + ", " + src(s[1]) + ")");
        case Slt: need(2); return assign(in, "vec4(lessThan(" + src(s[0]) + ", " + src(s[1]) + "))");
        case Sge: need(2); return assign(in, "vec4(greaterThanEqual(" + src(s[0]) + ", " + src(s[1]) + "))");
        case Exp: case ExpP: need(1); return assign(in, "vec4(exp2(" + scalar(s[0]) + "))");
        case Log: case LogP: need(1); return assign(in, "vec4(log2(abs(" + scalar(s[0]) + ")))");
        case Lit: {
            need(1);
            const std::string a = src(s[0]);
            return assign(in, "vec4(1.0, max((" + a + ").x, 0.0), ((" + a + ").x > 0.0 && (" + a + ").y > 0.0) ? pow((" + a +
                                  ").y, clamp((" + a + ").w, -127.9961, 127.9961)) : 0.0, 1.0)");
        }
        case DstOp: need(2); return assign(in, "vec4(1.0, (" + src(s[0]) + ").y * (" + src(s[1]) + ").y, (" + src(s[0]) + ").z, (" + src(s[1]) + ").w)");
        case Lrp: need(3); return assign(in, "mix(" + src(s[2]) + ", " + src(s[1]) + ", " + src(s[0]) + ")");
        case Frc: need(1); return assign(in, "fract(" + src(s[0]) + ")");
        case M4x4: need(2); return matrix(in, 4, 4);
        case M4x3: need(2); return matrix(in, 3, 4);
        case M3x4: need(2); return matrix(in, 4, 3);
        case M3x3: need(2); return matrix(in, 3, 3);
        case M3x2: need(2); return matrix(in, 2, 3);
        case Pow: need(2); return assign(in, "vec4(pow(abs(" + scalar(s[0]) + "), " + scalar(s[1]) + "))");
        case Crs: need(2); return assign(in, "vec4(cross((" + src(s[0]) + ").xyz, (" + src(s[1]) + ").xyz), 0.0)");
        case Sgn: need(1); return assign(in, "sign(" + src(s[0]) + ")");
        case Abs: need(1); return assign(in, "abs(" + src(s[0]) + ")");
        case Nrm: {
            need(1);
            const std::string a = src(s[0]);
            return assign(in, "(dot((" + a + ").xyz, (" + a + ").xyz) > 0.0 ? " + a + " * inversesqrt(dot((" + a + ").xyz, (" + a +
                                  ").xyz)) : vec4(0.0))");
        }
        case SinCos: need(1); return assign(in, "vec4(cos(" + scalar(s[0]) + "), sin(" + scalar(s[0]) + "), 0.0, 0.0)");
        case MovA:
            need(1);
            return "a0" + mask(in.dst.mask) + " = ivec4(floor(" + src(s[0]) + " + 0.5))" + mask(in.dst.mask) + ";";
        case Cmp: need(3); return assign(in, "mix(" + src(s[2]) + ", " + src(s[1]) + ", greaterThanEqual(" + src(s[0]) + ", vec4(0.0)))");
        case Dp2Add: need(3); return assign(in, "vec4(dot((" + src(s[0]) + ").xy, (" + src(s[1]) + ").xy) + " + scalar(s[2]) + ")");
        case Dsx: need(1); return assign(in, "dFdx(" + src(s[0]) + ")");
        case Dsy: need(1); return assign(in, "(OU_DFDY_SIGN * dFdy(" + src(s[0]) + "))");
        case Tex: {
            need(2);
            const std::string smp = sampler(s[1]);
            if (in.control == 1) {
                if (out_.samplerType[s[1].num & 15] != 2) throw Fail("projected fetch from non-2D sampler");
                return texResult(in, "textureProj(" + smp + ", " + src(s[0]) + ")");
            }
            if (in.control == 2) return texResult(in, "texture(" + smp + ", " + coords(s[0], s[1]) + ", (" + src(s[0]) + ").w)");
            return texResult(in, "texture(" + smp + ", " + coords(s[0], s[1]) + ")");
        }
        case TexLdl: need(2); return texResult(in, "textureLod(" + sampler(s[1]) + ", " + coords(s[0], s[1]) + ", (" + src(s[0]) + ").w)");
        case TexLdd: {
            need(4);
            const uint8_t t = out_.samplerType[s[1].num & 15];
            const char* c = t == 3 || t == 4 ? ".xyz" : ".xy";
            return texResult(in, "textureGrad(" + sampler(s[1]) + ", " + coords(s[0], s[1]) + ", (" + src(s[2]) + ")" + c + ", (" +
                                     src(s[3]) + ")" + c + ")");
        }
        case TexKill: {
            const std::string m = mask(in.dst.mask);
            return "if (any(lessThan((" + dstName(in.dst) + ")" + m + ", vec" + std::to_string(m.size() - 1) + "(0.0)))) discard;";
        }
        case SetP: {
            need(2);
            const std::string m = mask(in.dst.mask);
            return "p0" + m + " = " + cmpFn(in.control) + "(" + src(s[0]) + ", " + src(s[1]) + ")" + m + ";";
        }
        case If:
            need(1);
            if (s[0].type == Reg::ConstBool) return "if (" + boolExpr(s[0].num) + ") {";
            return "if (" + predicate(s[0]) + ") {";
        case IfC: need(2); return "if (" + scalar(s[0]) + " " + cmpOp(in.control) + " " + scalar(s[1]) + ") {";
        case Else: return "} else {";
        case EndIf: case EndLoop: case EndRep: return "}";
        case Break: return "break;";
        case BreakC: need(2); return "if (" + scalar(s[0]) + " " + cmpOp(in.control) + " " + scalar(s[1]) + ") break;";
        case BreakP: need(1); return "if (" + predicate(s[0]) + ") break;";
        case Rep: {
            need(1);
            const std::string i = "ouRep" + std::to_string(loops++);
            return "for (int " + i + " = 0; " + i + " < int(" + regValue(s[0]) + ".x); ++" + i + ") {";
        }
        case Loop: {
            need(2);
            const std::string i = "ouLoop" + std::to_string(loops++), v = regValue(s[1]);
            return "aL = int(" + v + ".y); for (int " + i + " = 0; " + i + " < int(" + v + ".x); ++" + i + ", aL += int(" + v + ".z)) {";
        }
        default: throw Fail(std::string("unsupported opcode ") + opName(in.op));
        }
    }
};

} // namespace

Glsl translate(const Program& p, bool vulkan) {
    Glsl g;
    try {
        if (p.major != 3) throw Fail("only shader model 3 is translated");
        Translator t(p, g);
        std::string body = t.run();
        g.source = vulkan ? body.insert(body.find('\n') + 1, "#define OU_VULKAN 1\n") : body;
    } catch (const std::exception& e) {
        g.source.clear();
        g.error = e.what();
    }
    return g;
}

std::vector<uint32_t> compileSpirv(const std::string& glsl, bool pixel, bool vulkan, std::string& error) {
    shaderc_compiler_t c = shaderc_compiler_initialize();
    shaderc_compile_options_t o = shaderc_compile_options_initialize();
    if (vulkan) shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    else shaderc_compile_options_set_target_env(o, shaderc_target_env_opengl, shaderc_env_version_opengl_4_5);
    shaderc_compile_options_set_optimization_level(o, shaderc_optimization_level_performance);
    shaderc_compilation_result_t r = shaderc_compile_into_spv(c, glsl.data(), glsl.size(),
                                                              pixel ? shaderc_glsl_fragment_shader : shaderc_glsl_vertex_shader,
                                                              pixel ? "tru.frag" : "tru.vert", "main", o);
    std::vector<uint32_t> spv;
    if (shaderc_result_get_compilation_status(r) == shaderc_compilation_status_success) {
        const auto* w = reinterpret_cast<const uint32_t*>(shaderc_result_get_bytes(r));
        spv.assign(w, w + shaderc_result_get_length(r) / 4);
    } else {
        error = shaderc_result_get_error_message(r);
    }
    shaderc_result_release(r);
    shaderc_compile_options_release(o);
    shaderc_compiler_release(c);
    return spv;
}

} // namespace ou::render::sm3
