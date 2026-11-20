// Dumps per-unit progression data from the game files for tools/re/chapter_data.py (docs/CHECKPOINTS.md):
//   checkpoint <unit> <uid> <object> <x> <y> <z> <yaw deg>  every `checkpoint` / `invis_checkpoint` intro (unit frame,
//                                                          tru.exe 0x5D40C0 respawns on them; yaw as game::Checkpoint)
//   condition  <unit> <hint_NN>                            help conditions a unit's scripts name ("hint_NN" strings in
//                                                          its DRM; trigger volumes hand them to the unit script, AUDIO.md §9.5)
//   line       <stream> <dtp> <English subtitle>           generalbank help-on-demand lines (single-stream DTPs 200..999)
// Units: pc-w\unitlist.txt order. Tab separated; text with tabs / newlines flattened to spaces.
// Built by CMake as ou-chapter-data. Run: OU_GAME_DIR=... ou-chapter-data > chapter_data.tsv
#include "audio/Audio.hpp"
#include "audio/SoundPlex.hpp"
#include "cdc/Cdc.hpp"
#include "formats/Formats.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <numbers>
#include <set>
#include <string>
#include <vector>

using namespace ou;

namespace {

std::vector<std::string> unitNames(const cdc::BigfileSet& bf) {
    std::vector<std::string> units;
    const auto ul = bf.read("\\tr8\\pc-w\\unitlist.txt");
    if (!ul) return units;
    const std::string text(ul->begin(), ul->end());
    for (size_t pos = text.find('\n') + 1; pos < text.size();) { // first line: the count
        const size_t nl = std::min(text.find('\n', pos), text.size());
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) units.push_back(line.substr(line.rfind('\\') + 1));
    }
    return units;
}

std::string flat(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

} // namespace

int main() {
    const char* dir = std::getenv("OU_GAME_DIR");
    if (!dir) {
        std::fprintf(stderr, "ou-chapter-data: set OU_GAME_DIR\n");
        return 2;
    }
    try {
        const cdc::BigfileSet bf(dir);
        const auto list = bf.read("\\tr8\\pc-w\\objectlist.txt");
        if (!list) throw std::runtime_error("objectlist.txt missing");
        const auto names = formats::parseObjectList({reinterpret_cast<const char*>(list->data()), list->size()});
        for (const auto& unit : unitNames(bf)) {
            auto bytes = bf.read("pc-w\\" + unit + ".drm");
            if (!bytes) continue;
            const cdc::Drm drm = cdc::parseDrm(std::move(*bytes));
            formats::Unit u;
            try {
                u = formats::parseUnit(drm);
            } catch (const std::exception&) {
                continue; // not a unit (object banks share the list in some installs)
            }
            for (const auto& in : u.intros) {
                const auto n = names.find(in.objectId);
                if (n == names.end() || (n->second != "checkpoint" && n->second != "invis_checkpoint")) continue;
                const float yaw = (in.rotation[2] - std::numbers::pi_v<float> / 2) * 180 / std::numbers::pi_v<float>;
                std::printf("checkpoint\t%s\t%d\t%s\t%.1f\t%.1f\t%.1f\t%.1f\n", unit.c_str(), in.uniqueId, n->second.c_str(),
                            in.position[0], in.position[1], in.position[2], std::remainder(yaw, 360.0f));
            }
            std::set<std::string> conditions;
            for (const auto& s : drm.sections) {
                const std::string_view d(reinterpret_cast<const char*>(s.data.data()), s.data.size());
                for (size_t at = d.find("hint_"); at != std::string_view::npos; at = d.find("hint_", at + 1))
                    if (at + 7 <= d.size() && std::isdigit(static_cast<unsigned char>(d[at + 5])) &&
                        std::isdigit(static_cast<unsigned char>(d[at + 6])) && (at + 7 == d.size() || d[at + 7] == '\0'))
                        conditions.insert(std::string(d.substr(at, 7)));
            }
            for (const auto& c : conditions) std::printf("condition\t%s\t%s\n", unit.c_str(), c.c_str());
        }
        audio::PlexBank bank;
        if (auto g = bf.read("pc-w\\generalbank.drm")) bank.add(std::make_shared<const cdc::Drm>(cdc::parseDrm(std::move(*g))));
        for (uint32_t id = 200; id < 1000; ++id) { // as app::GameSound's help lines (AUDIO.md §9.5)
            const audio::Plex* p = bank.find(id);
            if (!p || p->type != audio::PlexType::Assignment || p->children.size() != 1 || p->children[0].type != audio::PlexType::Stream)
                continue;
            const std::string& stream = p->children[0].stream;
            std::string text;
            if (auto m = bf.read("pc-w\\audio\\streams\\" + stream + ".mul")) {
                const audio::Mul mul = audio::decodeMul(*m);
                for (const auto& sub : mul.subtitles) {
                    const auto t = sub.text(0); // English
                    if (!t.empty()) text += (text.empty() ? "" : " ") + flat(t);
                }
            }
            std::printf("line\t%s\t%u\t%s\n", stream.c_str(), id, text.c_str());
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ou-chapter-data: %s\n", e.what());
        return 1;
    }
    return 0;
}
