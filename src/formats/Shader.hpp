// TRU shader libraries (DRM section type 9) and Direct3D 9 shader-model-3 bytecode.
// Layouts and evidence: docs/formats/SHADERS.md. The bytecode format is Microsoft's public
// D3D9 token format (d3d9types.h / "Direct3D Shader Codes"); this is our own parser.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace ou::formats::d3d9 {

// D3DSHADER_PARAM_REGISTER_TYPE
enum class Reg : uint8_t {
    Temp = 0, Input = 1, Const = 2, Addr = 3 /* ps: Texture */, RastOut = 4, AttrOut = 5,
    Output = 6 /* vs_3_0 o#, vs_2 texcrdout */, ConstInt = 7, ColorOut = 8, DepthOut = 9, Sampler = 10,
    Const2 = 11, Const3 = 12, Const4 = 13, ConstBool = 14, Loop = 15, TempFloat16 = 16, Misc = 17,
    Label = 18, Predicate = 19,
};

// D3DDECLUSAGE
enum class Usage : uint8_t {
    Position = 0, BlendWeight = 1, BlendIndices = 2, Normal = 3, PSize = 4, Texcoord = 5, Tangent = 6,
    Binormal = 7, TessFactor = 8, PositionT = 9, Color = 10, Fog = 11, Depth = 12, Sample = 13,
};

// D3DSIO_* opcodes used by SM2/SM3 (the parser accepts any value; names for unknown ones are "op<N>").
enum Op : uint16_t {
    Nop = 0, Mov, Add, Sub, Mad, Mul, Rcp, Rsq, Dp3, Dp4, Min, Max, Slt, Sge, Exp, Log, Lit, DstOp, Lrp, Frc,
    M4x4, M4x3, M3x4, M3x3, M3x2, Call, CallNZ, Loop, Ret, EndLoop, Label, Dcl, Pow, Crs, Sgn, Abs, Nrm,
    SinCos, Rep, EndRep, If, IfC, Else, EndIf, Break, BreakC, MovA, DefB, DefI,
    TexCoord = 64, TexKill, Tex, TexBem, TexBemL, TexReg2AR, TexReg2GB, TexM3x2Pad, TexM3x2Tex, TexM3x3Pad,
    TexM3x3Tex, Reserved0, TexM3x3Spec, TexM3x3VSpec, ExpP, LogP, Cnd, Def, TexReg2RGB, TexDp3Tex,
    TexM3x2Depth, TexDp3, TexM3x3, TexDepth, Cmp, Bem, Dp2Add, Dsx, Dsy, TexLdd, SetP, TexLdl, BreakP,
    Phase = 0xFFFD, Comment = 0xFFFE, End = 0xFFFF,
};
const char* opName(uint16_t op);
// True for opcodes the translator (render/ShaderTranslate) implements.
bool opSupported(uint16_t op);

struct Src {
    Reg type = Reg::Temp;
    uint16_t num = 0;
    uint8_t swizzle = 0xE4; // 2 bits per component, xyzw = 0xE4
    uint8_t mod = 0;        // D3DSPSM_*: 1 neg, 2 bias, 3 biasneg, 4 sign(bx2), 5 signneg, 6 comp, 7 x2, 8 x2neg, 9 dz, 10 dw, 11 abs, 12 absneg, 13 not
    bool relative = false;  // [num + rel] addressing
    Reg relType = Reg::Addr;
    uint16_t relNum = 0;
    uint8_t relComp = 0;    // component of the address register
};

struct Dst {
    Reg type = Reg::Temp;
    uint16_t num = 0;
    uint8_t mask = 0xF;     // xyzw bits
    uint8_t mod = 0;        // 1 saturate, 2 partial precision, 4 centroid
    int8_t shift = 0;       // result shift (SM1 only; 0 in SM3)
};

struct Instruction {
    uint16_t op = Nop;
    uint8_t control = 0;      // comparison (ifc/breakc/setp) or texld variant (1 project, 2 bias)
    bool predicated = false;
    Src pred;                 // when predicated
    bool hasDst = false;
    Dst dst;
    std::vector<Src> src;
    // dcl
    Usage usage = Usage::Position;
    uint8_t usageIndex = 0;
    uint8_t textureType = 0;  // D3DSAMPLER_TEXTURE_TYPE >> 27: 2 = 2D, 3 = cube, 4 = volume
    // def / defi / defb
    uint32_t value[4] = {};
};

// CTAB (D3DXSHADER_CONSTANTTABLE) entry.
struct Constant {
    std::string name;
    uint16_t registerSet = 0; // 0 bool, 1 int4, 2 float4, 3 sampler
    uint16_t registerIndex = 0, registerCount = 0;
};

struct Program {
    bool pixel = false;
    uint8_t major = 0, minor = 0;
    std::vector<Instruction> instructions; // without comments and the end token
    std::vector<Constant> constants;       // CTAB, empty when absent
    std::string creator;
    size_t tokenCount = 0;                 // including version and end tokens
    const Constant* constant(std::string_view name) const;
};

// Throws cdc::FormatError on malformed token streams (truncation, missing end token, bad lengths).
Program parse(std::span<const uint32_t> tokens);
Program parse(std::span<const uint8_t> bytes); // little-endian, size multiple of 4
std::string disassemble(const Program& p);
std::string format(const Src& s);
std::string format(const Dst& d);

// ---- TRU ShaderLib section (type 9) ------------------------------------------------------------
// data: {u32 kind (0 vertex, 1 pixel), table}; table: {u32 headerBytes, u32 ?, u32 offsets[headerBytes / 4]}
// with offsets relative to the table (~0 = no shader); each entry {u32 libHash, u16 size (incl. this header), u16 flags, u64 hash, bytecode[size - 16]}
// (tru.exe PCShaderLib fill 0x515530, PCVertexShaderTable 0x5263A0, PCPixelShaderTable 0x526440).
struct ShaderLibEntry {
    uint32_t index = 0;   // table slot (variant flags, SHADERS.md)
    uint32_t libHash = 0;
    uint16_t flags = 0;   // [U] 0 or 2 observed
    uint64_t hash = 0;
    std::span<const uint8_t> bytecode;
};
struct ShaderLib {
    bool pixel = false;
    uint32_t slots = 0;
    std::vector<ShaderLibEntry> entries; // present slots, ascending
    const ShaderLibEntry* find(uint32_t index) const;
};
ShaderLib parseShaderLib(std::span<const uint8_t> sectionData);

// Every distinct shader (keyed by kind + 64-bit entry hash) of every ShaderLib section in every DRM of the
// install's archives (patch.000 + bigfile.000.., all specialisations). Records are probed for CDRM / DRM v19.
// `hashMismatches` counts entries whose hash matched an earlier one with different bytecode (0 observed).
struct InstallShader {
    bool pixel = false;
    uint64_t hash = 0;
    std::vector<uint8_t> bytecode;
    uint32_t sectionId = 0, index = 0; // first occurrence
};
struct InstallShaders {
    std::vector<InstallShader> shaders;
    size_t drms = 0, libs = 0, entries = 0, hashMismatches = 0;
    std::vector<std::string> errors; // unparsable libs
};
InstallShaders collectInstallShaders(const std::string& gameDir);

} // namespace ou::formats::d3d9
