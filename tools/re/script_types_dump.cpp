// Dumps the native-backed script types of every unit DRM (section id, native name, per function index: vt slot,
// argument count, return type, native flag) and every CALLU (0x65) site with whether its Function operand resolves
// to a function of a type-8 section of the same DRM. Input for tools/re/script_natives.py (docs/re/SCRIPT_NATIVES.md).
//
// Build against an existing build tree (read-only use of its static libraries):
//   B=build-AuditBaseline; c++ -std=c++23 -O1 -Isrc -I$B/gen tools/re/script_types_dump.cpp \
//     $B/libou_script.a $B/libou_formats.a $B/libou_cdc.a -lz -o /tmp/script_types_dump
// Run: OU_GAME_DIR=... /tmp/script_types_dump > script_types.tsv
#include "cdc/Cdc.hpp"
#include "script/Script.hpp"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <tuple>

using namespace ou;

int main() {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) { std::fprintf(stderr, "OU_GAME_DIR not set\n"); return 2; }
    const cdc::BigfileSet bf(g);
    const auto list = bf.read("\\tr8\\pc-w\\unitlist.txt");
    if (!list) { std::fprintf(stderr, "no unitlist\n"); return 2; }
    std::string text(list->begin(), list->end());
    // type id -> (name, function rows); identical across units, first unit wins (mismatches reported)
    std::map<uint32_t, std::pair<std::string, std::vector<std::string>>> natives;
    std::map<uint32_t, std::set<std::string>> nativeUnits;
    // CALLU sites: (caller type, fn index, word, target section id, target section type, resolves, target fn) -> units
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, int, std::string>, std::set<std::string>> callu;
    std::set<std::string> mismatches;
    std::map<std::tuple<int, int, int>, std::set<uint32_t>> operands; // (op, exe operand words, decoded) -> types
    std::map<std::pair<uint32_t, size_t>, std::set<uint32_t>> nativeCalls; // native (type, fn index) -> calling types
    for (size_t pos = text.find('\n') + 1; pos < text.size();) {
        const size_t nl = std::min(text.find('\n', pos), text.size());
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        const std::string name = line.substr(line.rfind('\\') + 1);
        auto bytes = bf.read("pc-w\\" + name + ".drm");
        if (!bytes) continue;
        const cdc::Drm drm = cdc::parseDrm(std::move(*bytes));
        std::map<std::pair<uint32_t, uint32_t>, std::pair<uint32_t, size_t>> methods; // Function/Prototype -> (type, index)
        std::vector<script::ScriptType> types;
        for (uint32_t i = 0; i < drm.sections.size(); ++i)
            if (drm.sections[i].type == cdc::SectionType::Script) types.push_back(script::parseScriptType(drm, i));
        for (const auto& t : types)
            for (size_t j = 0; j < t.functions.size(); ++j) {
                methods[{t.functions[j].self.section, t.functions[j].self.offset}] = {t.id, j};
                methods[{t.functions[j].proto.section, t.functions[j].proto.offset}] = {t.id, j};
            }
        std::map<uint32_t, const script::ScriptType*> byId;
        for (const auto& t : types) byId[t.id] = &t;
        for (const auto& t : types) {
            if (!t.nativeName.empty()) {
                std::vector<std::string> rows;
                for (size_t j = 0; j < t.functions.size(); ++j) {
                    const auto& f = t.functions[j];
                    char buf[160];
                    std::snprintf(buf, sizeof buf, "%zu\t%d\t%u\t%u\t%d\t%u", j, int(f.vtIndex), unsigned(f.argCount),
                                  unsigned(f.returnType), int(f.native), unsigned(f.wordCount));
                    rows.push_back(buf);
                }
                auto [it, fresh] = natives.try_emplace(t.id, t.nativeName, rows);
                if (!fresh && (it->second.first != t.nativeName || it->second.second != rows))
                    mismatches.insert(std::to_string(t.id) + " " + name);
                nativeUnits[t.id].insert(name);
            }
            // OU_DUMP_TYPE=<id>: list that type's decoded instructions once (op, arg, word, operands)
            static bool dumped = false;
            if (const char* d = std::getenv("OU_DUMP_TYPE"); d && uint32_t(std::atoi(d)) == t.id && !dumped) {
                dumped = true;
                for (size_t j = 0; j < t.functions.size(); ++j)
                    for (const auto& in : t.functions[j].code) {
                        std::printf("#ins\t%u\t%zu\tvt=%d\t%u\t0x%02X\t%d\tref=%u\timm=0x%X", t.id, j, int(t.functions[j].vtIndex),
                                    in.word, in.op, in.arg, in.ref, in.imm);
                        for (const auto& p : in.ptrs) {
                            const auto m = methods.find({p.section, p.offset});
                            std::printf("\tptr=%u+0x%X%s", p.section < drm.sections.size() ? drm.sections[p.section].id : 0, p.offset,
                                        m != methods.end() ? (" fn " + std::to_string(m->second.first) + ":" + std::to_string(m->second.second)).c_str() : "");
                        }
                        std::printf("\n");
                    }
            }
            for (size_t j = 0; j < t.functions.size(); ++j)
                for (const auto& in : t.functions[j].code) {
                    if (in.op != script::kOpCallU) continue;
                    uint32_t secId = 0; int secType = -1; std::string target = "-";
                    if (!in.ptrs.empty()) {
                        const auto p = in.ptrs[0];
                        if (p.section < drm.sections.size()) {
                            secId = drm.sections[p.section].id;
                            secType = int(drm.sections[p.section].type);
                        }
                        const auto m = methods.find({p.section, p.offset});
                        if (m != methods.end()) target = std::to_string(m->second.first) + ":" + std::to_string(m->second.second);
                        else target = "unresolved@" + std::to_string(p.offset);
                    } else {
                        // No pointer relocation: report the raw operand word and which other relocation kind
                        // (kind2 / resource32 / resource16, CONTAINERS.md 4.5) covers it, if any.
                        target = "no-operand";
                        const auto& fs = t.functions[j].self;
                        const auto fptrs = cdc::sectionPointers(drm, fs.section);
                        if (const auto cp = fptrs.find(fs.offset + 0x18); cp != fptrs.end()) {
                            const auto& cs = drm.sections[cp->second.section];
                            const uint32_t off = cp->second.offset + 4 * (in.word + 1);
                            const uint32_t raw = off + 4 <= cs.data.size() ? *reinterpret_cast<const uint32_t*>(cs.data.data() + off) : 0;
                            std::string kind = "none";
                            const auto r = cs.relocations;
                            if (r.size() >= 20) {
                                uint32_t c[5];
                                for (int i = 0; i < 5; ++i) c[i] = *reinterpret_cast<const uint32_t*>(r.data() + 4 * i);
                                size_t p = 20 + 8 * size_t(c[0]) + 8 * size_t(c[1]);
                                const char* names[3] = {"kind2", "resource32", "resource16"};
                                for (int k = 0; k < 3; ++k)
                                    for (uint32_t e = 0; e < c[2 + k]; ++e, p += 4) {
                                        const uint32_t v = *reinterpret_cast<const uint32_t*>(r.data() + p);
                                        if ((v & 0x1FFFFFF) * 4 == off) kind = std::string(names[k]) + "/type" + std::to_string(v >> 25);
                                    }
                            }
                            char buf[96];
                            std::snprintf(buf, sizeof buf, "raw=0x%08X reloc=%s", raw, kind.c_str());
                            target = buf;
                        }
                    }
                    callu[{t.id, uint32_t(j), in.word, secId, secType, target}].insert(name);
                }
            // Operand words per opcode as tru.exe's link pass 0x57EE20 steps them (one word: 02 05 34 35 3B 3C 65 66
            // 68-6C 6E-72 74-77 7E 7F; two words: 67 6D 73) vs the words our decoder consumed (relocations / PUSH).
            for (size_t j = 0; j < t.functions.size(); ++j) {
                const auto& code = t.functions[j].code;
                for (size_t k = 0; k < code.size(); ++k) {
                    const uint8_t op = code[k].op;
                    const int expect = (op == 0x67 || op == 0x6D || op == 0x73) ? 2 :
                        (op == 0x02 || op == 0x05 || op == 0x34 || op == 0x35 || op == 0x3B || op == 0x3C || op == 0x65 ||
                         op == 0x66 || (op >= 0x68 && op <= 0x6C) || (op >= 0x6E && op <= 0x72) || (op >= 0x74 && op <= 0x77) ||
                         op == 0x7E || op == 0x7F) ? 1 : 0;
                    const uint32_t next = k + 1 < code.size() ? code[k + 1].word : t.functions[j].wordCount;
                    const int got = int(next - code[k].word - 1);
                    if (got != expect) operands[{op, expect, got}].insert(t.id);
                }
            }
            // Native functions the bytecode references (CALLU / CALLn / SSEQ operands that resolve to a native record)
            for (const auto& f : t.functions)
                for (const auto& in : f.code)
                    for (const auto& p : in.ptrs) {
                        const auto m = methods.find({p.section, p.offset});
                        if (m == methods.end()) continue;
                        const auto ti = byId.find(m->second.first);
                        if (ti != byId.end() && ti->second->functions[m->second.second].native)
                            nativeCalls[m->second].insert(t.id);
                    }
        }
    }
    std::printf("#natives\ttype_id\tnative_name\tfn_index\tvt\targs\tret\tnative\twords\tunits\n");
    for (const auto& [id, v] : natives)
        for (const auto& r : v.second)
            std::printf("N\t%u\t%s\t%s\t%zu\n", id, v.first.c_str(), r.c_str(), nativeUnits[id].size());
    std::printf("#calls\ttype_id\tfn_index\tcalling_types\n");
    for (const auto& [k, ids] : nativeCalls) std::printf("R\t%u\t%zu\t%zu\n", k.first, k.second, ids.size());
    std::printf("#callu\tcaller_type\tcaller_fn\tword\ttarget_section_id\ttarget_section_type\ttarget\tunits\n");
    for (const auto& [k, units] : callu)
        std::printf("C\t%u\t%u\t%u\t%u\t%d\t%s\t%zu\n", std::get<0>(k), std::get<1>(k), std::get<2>(k), std::get<3>(k),
                    std::get<4>(k), std::get<5>(k).c_str(), units.size());
    std::printf("#operands\top\texe_words\tdecoded_words\tdistinct_types\n");
    for (const auto& [k, ids] : operands)
        std::printf("O\t0x%02X\t%d\t%d\t%zu\n", std::get<0>(k), std::get<1>(k), std::get<2>(k), ids.size());
    for (const auto& m : mismatches) std::printf("#mismatch\t%s\n", m.c_str());
}
