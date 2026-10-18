// ou-audio: list, decode and play TRU audio (.mul streams, FSB4 banks, DRM Wave sections).
#include "audio/Mixer.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <optional>
#include <thread>
#include <vector>

using namespace ou;
namespace fs = std::filesystem;

namespace {

int usage() {
    std::cerr << "usage:\n"
                 "  ou-audio list   <gameDir> <path> [--lang en|fr|de|it|es]\n"
                 "  ou-audio decode <gameDir> <path> -o out.wav [--id N] [--lang L]\n"
                 "  ou-audio play   <gameDir> <path> [--id N] [--lang L] [--seconds S]\n"
                 "  ou-audio scan   <gameDir>   (decode every audio record and Wave section)\n"
                 "<path> is a bigfile path: a .mul stream, an FSB4 bank (.sam) or a .drm (Wave sections;\n"
                 "--id = section id). For FSB4 banks --id is the sample index.\n";
    return 2;
}

uint32_t langMask(const std::string& l) {
    static const std::map<std::string, uint32_t> m{{"en", cdc::kSpecEnglish}, {"fr", cdc::kSpecFrench},
                                                   {"de", cdc::kSpecGerman}, {"it", cdc::kSpecItalian},
                                                   {"es", cdc::kSpecSpanish}};
    const auto it = m.find(l);
    if (it == m.end()) throw std::runtime_error("unknown language " + l);
    return it->second;
}

bool endsWith(const std::string& s, const char* e) {
    const size_t n = std::strlen(e);
    return s.size() >= n && std::equal(s.end() - long(n), s.end(), e, [](char a, char b) { return std::tolower(a) == b; });
}

void writeWav(const fs::path& path, const audio::Pcm& p) {
    std::ofstream o(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { o.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { o.write(reinterpret_cast<const char*>(&v), 2); };
    const uint32_t bytes = uint32_t(p.samples.size() * 2);
    o.write("RIFF", 4); u32(36 + bytes); o.write("WAVEfmt ", 8); u32(16);
    u16(1); u16(p.channels); u32(p.sampleRate); u32(p.sampleRate * p.channels * 2u); u16(uint16_t(p.channels * 2)); u16(16);
    o.write("data", 4); u32(bytes);
    o.write(reinterpret_cast<const char*>(p.samples.data()), bytes);
    if (!o) throw std::runtime_error("cannot write " + path.string());
}

double rmsDb(const audio::Pcm& p) {
    double acc = 0;
    for (int16_t s : p.samples) acc += double(s) * s;
    const double rms = p.samples.empty() ? 0 : std::sqrt(acc / double(p.samples.size())) / 32768.0;
    return rms > 0 ? 20 * std::log10(rms) : -999;
}

void describe(const audio::Pcm& p) {
    std::printf("  %u Hz, %u ch, %zu frames (%.3f s), loopStart %lld, RMS %.1f dBFS\n", p.sampleRate, p.channels, p.frames(),
                p.sampleRate ? double(p.frames()) / p.sampleRate : 0.0, static_cast<long long>(p.loopStart), rmsDb(p));
}

// Decodes `path` (+ --id) to PCM; prints a description when `verbose`.
audio::Pcm load(const cdc::BigfileSet& bf, const std::string& path, std::optional<uint32_t> id, bool verbose) {
    auto bytes = bf.read(path);
    if (!bytes) throw std::runtime_error("not in bigfile: " + path);
    if (audio::isFsb4(*bytes)) {
        const auto bank = audio::parseFsb4(*bytes);
        if (verbose)
            for (size_t i = 0; i < bank.samples.size(); ++i) {
                const auto& s = bank.samples[i];
                std::printf("sample %zu '%s' mode 0x%X %d Hz %u ch %u samples %u bytes\n", i, s.name.c_str(), s.mode,
                            s.defaultRate, s.channels, s.numSamples, s.dataSize);
            }
        const uint32_t i = id.value_or(0);
        if (i >= bank.samples.size()) throw std::runtime_error("no such sample");
        return audio::decodeFsb4(*bytes, bank.samples[i]);
    }
    if (endsWith(path, ".drm")) {
        const auto drm = cdc::parseDrm(std::move(*bytes));
        std::optional<audio::Pcm> found;
        for (const auto& s : drm.sections) {
            if (s.type != cdc::SectionType::Wave) continue;
            if (verbose) {
                const auto w = audio::parseWave(s.data);
                const auto& f = w.bank.samples[0];
                std::printf("wave 0x%X '%s' %u Hz %u samples (%.3f s) loop %u..%u unk %u mode 0x%X\n", s.id, f.name.c_str(),
                            w.sampleRate, f.numSamples, double(f.numSamples) / w.sampleRate, w.loopStart, w.loopEnd, w.unknown,
                            f.mode);
            }
            if (id && s.id == *id) found = audio::decodeWave(s.data);
        }
        if (!id) return {};
        if (!found) throw std::runtime_error("no Wave section with that id");
        return *found;
    }
    auto m = audio::decodeMul(*bytes);
    if (verbose) {
        const auto& h = m.header;
        std::printf("mul: %u Hz, %u ch, %u samples, loopStart %d, mediaLength %.0f frames, cinematic %u, subtitles %u,"
                    " cine packets %u%s\n  volL/volR:",
                    h.sampleRate, h.channels, h.numSamples, h.loopStart, h.mediaLength, h.hasCinematic, h.hasSubtitles,
                    m.cinematicPackets, m.sceneFrames ? " (scene frames: not parsed)" : "");
        for (uint32_t c = 0; c < h.channels; ++c) std::printf(" %.2g/%.2g", h.volLeft[c], h.volRight[c]);
        std::printf("\n");
        for (const auto& s : m.subtitles) {
            std::printf("subtitle @ frame %u (%.2f s):\n", s.frame, s.frame / 30.0);
            for (const auto& [l, t] : s.lines) std::printf("  [%u] %s\n", l, t.c_str());
        }
    }
    return std::move(m.pcm);
}

int scan(const fs::path& game) {
    struct Kind { size_t ok = 0, fail = 0; double seconds = 0; };
    std::map<std::string, Kind> kinds;
    std::map<std::string, size_t> facts;
    std::vector<std::string> failures;
    std::vector<fs::path> archives{game / "bigfile.000"};
    if (fs::exists(game / "patch.000")) archives.push_back(game / "patch.000");
    for (const auto& a : archives) {
        const cdc::Bigfile bf(a);
        for (const auto& r : bf.records()) {
            std::vector<uint8_t> d = bf.read(r);
            std::string kind;
            char tag[64];
            try {
                if (audio::isFsb4(d)) {
                    kind = "FSB4";
                    const auto bank = audio::parseFsb4(d);
                    for (const auto& s : bank.samples) {
                        const auto p = audio::decodeFsb4(d, s);
                        kinds[kind].seconds += double(p.frames()) / p.sampleRate;
                        std::snprintf(tag, sizeof tag, "FSB4 '%s' mode 0x%X %u samples", s.name.c_str(), s.mode, s.numSamples);
                        ++facts[tag];
                    }
                } else if (d.size() >= 4 && std::memcmp(d.data(), "Mus!", 4) == 0) {
                    kind = "Mus!";
                } else if (cdc::isCdrm(d) || (d.size() >= 4 && d[0] == 19 && !d[1] && !d[2] && !d[3])) {
                    kind = "DRM";
                    const auto drm = cdc::parseDrm(std::move(d));
                    for (const auto& s : drm.sections) {
                        if (s.type != cdc::SectionType::Wave) continue;
                        try {
                            const auto w = audio::parseWave(s.data);
                            const auto p = audio::decodeWave(s.data);
                            ++kinds["Wave"].ok;
                            kinds["Wave"].seconds += double(p.frames()) / p.sampleRate;
                            const auto& f = w.bank.samples[0];
                            std::snprintf(tag, sizeof tag, "Wave mode 0x%X ch %u fsbRate %d unk %u", f.mode, f.channels,
                                          f.defaultRate, w.unknown);
                            ++facts[tag];
                            if (w.loopEnd) ++facts[w.loopStart < w.loopEnd && w.loopEnd <= f.numSamples ? "Wave loop points valid"
                                                                                                    : "Wave loop points INVALID"];
                        } catch (const std::exception& e) {
                            ++kinds["Wave"].fail;
                            if (failures.size() < 20) failures.push_back("wave " + std::to_string(s.id) + ": " + e.what());
                        }
                    }
                } else if (d.size() >= audio::kMulHeaderSize && !d[0x10] && !d[0x14] && !d[0x18] && !d[0x1C] &&
                           [&] { try { audio::parseMulHeader(d); return true; } catch (...) { return false; } }()) {
                    kind = "MUL";
                    const auto m = audio::decodeMul(d);
                    kinds[kind].seconds += double(m.pcm.frames()) / m.pcm.sampleRate;
                    std::snprintf(tag, sizeof tag, "MUL %u ch", m.header.channels);
                    ++facts[tag];
                    if (m.subtitles.size()) ++facts["MUL with subtitles"];
                    if (m.sceneFrames) ++facts["MUL with scene frames"];
                    if (m.header.loopStart >= 0) ++facts["MUL looping"];
                    if (m.pcm.frames() < m.header.numSamples) ++facts["MUL shorter than header numSamples"];
                } else {
                    continue;
                }
                ++kinds[kind].ok;
            } catch (const std::exception& e) {
                ++kinds[kind].fail;
                char h[16];
                std::snprintf(h, sizeof h, "%08X", r.hash);
                if (failures.size() < 20) failures.push_back(kind + " " + h + ": " + e.what());
            }
        }
    }
    for (const auto& [k, v] : kinds) std::printf("%-5s ok %zu fail %zu  %.1f s decoded\n", k.c_str(), v.ok, v.fail, v.seconds);
    for (const auto& [k, v] : facts) std::printf("  %6zu  %s\n", v, k.c_str());
    for (const auto& f : failures) std::printf("FAIL %s\n", f.c_str());
    size_t fails = 0;
    for (const auto& [k, v] : kinds) fails += v.fail;
    return fails ? 1 : 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string cmd = argv[1];
    const fs::path game = argv[2];
    std::vector<std::string> pos;
    std::optional<uint32_t> id;
    std::string out, lang = "en";
    double seconds = 0;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
            return argv[++i];
        };
        if (a == "-o") out = next();
        else if (a == "--id") id = uint32_t(std::stoul(next(), nullptr, 0));
        else if (a == "--lang") lang = next();
        else if (a == "--seconds") seconds = std::stod(next());
        else pos.push_back(a);
    }
    try {
        if (cmd == "scan") return scan(game);
        if (pos.empty()) return usage();
        const cdc::BigfileSet bf(game, langMask(lang));
        if (cmd == "list") {
            auto p = load(bf, pos[0], id, true);
            if (p.channels) describe(p);
            return 0;
        }
        if (cmd == "decode") {
            if (out.empty()) return usage();
            auto p = load(bf, pos[0], id, false);
            if (!p.channels) throw std::runtime_error("nothing to decode (missing --id?)");
            writeWav(out, p);
            describe(p);
            std::printf("wrote %s\n", out.c_str());
            return 0;
        }
        if (cmd == "play") {
            auto p = std::make_shared<const audio::Pcm>(load(bf, pos[0], id, false));
            if (!p->channels) throw std::runtime_error("nothing to play (missing --id?)");
            describe(*p);
            audio::Mixer mixer;
            if (!mixer.openDevice()) throw std::runtime_error(std::string("SDL audio: ") + SDL_GetError());
            const double len = double(p->frames()) / p->sampleRate;
            const double limit = seconds > 0 ? std::min(seconds, len) : len;
            if (p->channels > 2) mixer.playMusic(p, 0); // stem pairs mixed per header gains
            else mixer.play(p);
            const auto t0 = std::chrono::steady_clock::now();
            auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
            while (elapsed() < limit && mixer.activeVoices()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            std::printf("played %.2f s through SDL driver '%s' at %u Hz\n", elapsed(), SDL_GetCurrentAudioDriver(),
                        mixer.outputRate());
            return 0;
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return usage();
}
