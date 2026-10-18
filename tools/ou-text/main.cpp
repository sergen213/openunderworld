// ou-text: dump localized strings (locals.bin), movie subtitles (.sch) and cinematic subtitles (cinstream .mul).
#include <cstdio>
#include <cstdlib>
#include <string>

#include "cdc/Cdc.hpp"
#include "cine/Cine.hpp"
#include "locale/Locale.hpp"

namespace fs = std::filesystem;
using namespace ou;

namespace {

void usage() {
    std::fputs("usage: ou-text dump  [--lang en] [--game DIR]   id<TAB>text of every string (\\n escaped)\n"
               "       ou-text get <id> [--lang en] [--game DIR]\n"
               "       ou-text langs [--game DIR]               locals.bin variants: header id, count, invalid\n"
               "       ou-text movie <name> [--lang en]         subtitle cues of Movies/<name>.bik (.sch)\n"
               "       ou-text cine <name> [--lang en|all]      subtitles of pc-w\\cinstream\\<name>.mul\n"
               "Languages: en fr de it es ja pt pl en-gb ru cs nl hu (or 0..12). --game defaults to $OU_GAME_DIR.\n",
               stderr);
}

std::string escaped(std::string_view s) {
    std::string out;
    for (char c : s) out += c == '\n' ? std::string("\\n") : c == '\r' ? std::string("\\r") : std::string(1, c);
    return out;
}

} // namespace

int main(int argc, char** argv) try {
    std::vector<std::string> pos;
    fs::path game = std::getenv("OU_GAME_DIR") ? std::getenv("OU_GAME_DIR") : "";
    std::string langArg = "en";
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--lang" && i + 1 < argc) langArg = argv[++i];
        else if (a == "--game" && i + 1 < argc) game = argv[++i];
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else pos.push_back(a);
    }
    if (pos.empty()) { usage(); return 2; }
    if (game.empty()) throw std::runtime_error("no game directory (--game or OU_GAME_DIR)");
    const bool allLangs = langArg == "all";
    const auto lang = allLangs ? locale::Language::English : locale::parseLanguage(langArg).value_or(locale::Language(-1));
    if (static_cast<int>(lang) < 0) throw std::runtime_error("unknown language " + langArg);
    const std::string& cmd = pos[0];

    if (cmd == "dump" || cmd == "get") {
        auto s = locale::loadStrings(game, lang);
        if (!s) throw std::runtime_error("no locals.bin for " + langArg);
        if (cmd == "get") {
            if (pos.size() < 2) { usage(); return 2; }
            std::printf("%s\n", std::string(s->get(std::stoi(pos[1]))).c_str());
            return 0;
        }
        for (size_t i = 0; i < s->size(); ++i) {
            const std::string_view t = s->get(int(i));
            if (!t.empty()) std::printf("%zu\t%s\n", i, escaped(t).c_str());
        }
    } else if (cmd == "langs") {
        for (int l = 0; l < locale::kLanguageCount; ++l) {
            auto s = locale::loadStrings(game, locale::Language(l));
            if (!s) { std::printf("%-15s -\n", std::string(locale::languageName(locale::Language(l))).c_str()); continue; }
            size_t invalid = 0, empty = 0;
            for (size_t i = 0; i < s->size(); ++i) {
                if (s->get(int(i)).empty()) ++empty;
                else if (!s->valid(int(i))) ++invalid;
            }
            std::printf("%-15s header %u, %zu strings (%zu empty, %zu invalid), [1] = %s\n",
                        std::string(locale::languageName(locale::Language(l))).c_str(), s->headerLanguage(), s->size(),
                        empty, invalid, std::string(s->get(1)).c_str());
        }
    } else if (cmd == "movie" && pos.size() >= 2) {
        auto sch = cdc::BigfileSet(game).read(locale::movieSchemaPath(pos[1]));
        if (!sch) throw std::runtime_error("no subtitle schema for movie " + pos[1]);
        for (const auto& c : locale::parseMovieSchema({reinterpret_cast<const char*>(sch->data()), sch->size()}, lang))
            std::printf("%8.3f\t%s\n", c.start, escaped(c.text).c_str());
    } else if (cmd == "cine" && pos.size() >= 2) {
        auto mul = cdc::BigfileSet(game, locale::specMask(lang)).read("pc-w\\cinstream\\" + pos[1] + ".mul");
        if (!mul) throw std::runtime_error("no pc-w\\cinstream\\" + pos[1] + ".mul");
        const auto info = cine::scan(cine::demux(*mul));
        std::printf("# unit %s, %d frames (%.2f s at 30 fps), %u channels, %zu subtitle entries\n", info.header.unitName.c_str(),
                    info.frames, info.frames / 30.0, info.header.channels, info.subtitles.size());
        for (const auto& s : info.subtitles)
            if (allLangs || s.language == static_cast<int>(lang))
                std::printf("%6d\t%8.3f\t%2d\t%s\n", s.frame, s.frame / 30.0, s.language, escaped(s.text).c_str());
    } else {
        usage();
        return 2;
    }
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "ou-text: %s\n", e.what());
    return 1;
}
