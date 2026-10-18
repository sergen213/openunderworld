// ou-movie: play a Movies/*.bik natively (SDL3 window + audio, subtitles from the game's .sch).
#include <cstdio>
#include <cstdlib>
#include <string>

#include <SDL3/SDL.h>

#include "cdc/Cdc.hpp"
#include "locale/Locale.hpp"
#include "platform/Sdl.hpp"
#include "video/Movie.hpp"
#include "video/Player.hpp"

namespace fs = std::filesystem;
using namespace ou;

namespace {

void usage() {
    std::fputs("usage: ou-movie <file.bik> [--frames N --screenshot out.ppm] [--lang en] [--track ID]\n"
               "                [--no-audio] [--no-subs] [--font file.ttf] [--game DIR] [--stats]\n"
               "       ou-movie --info <file.bik|dir>   container/stream inventory\n"
               "--lang picks subtitles and the audio track (tru.exe: track id = language index, else first).\n"
               "Subtitles come from \\tr8\\local\\localization\\movies\\<name>.sch in the game's bigfiles\n"
               "(--game, else $OU_GAME_DIR, else the movie's grandparent directory).\n"
               "Any key, mouse or gamepad button skips. Exit 77: no display.\n",
               stderr);
}

void printInfo(const fs::path& file) {
    video::Movie m(file, -1);
    const auto& i = m.info();
    std::printf("%s: %s, %s %dx%d @ %.5f fps, %.3f s, %zu audio track(s)\n", file.filename().c_str(), i.format.c_str(),
                i.videoCodec.c_str(), i.width, i.height, i.fps, i.duration, i.audio.size());
    for (const auto& a : i.audio)
        std::printf("  stream %d: track id %d, %s %d Hz %d ch\n", a.stream, a.id, a.codec.c_str(), a.sampleRate, a.channels);
}

} // namespace

int main(int argc, char** argv) try {
    fs::path file, screenshot, font, gameDir;
    int frames = -1, track = -2;
    bool info = false, noAudio = false, noSubs = false, stats = false;
    locale::Language lang = locale::Language::English;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(a + " needs a value");
            return argv[++i];
        };
        if (a == "--frames") frames = std::stoi(val());
        else if (a == "--screenshot") screenshot = val();
        else if (a == "--track") track = std::stoi(val());
        else if (a == "--font") font = val();
        else if (a == "--game") gameDir = val();
        else if (a == "--info") info = true;
        else if (a == "--no-audio") noAudio = true;
        else if (a == "--no-subs") noSubs = true;
        else if (a == "--stats") stats = true;
        else if (a == "--lang") {
            const std::string v = val();
            auto l = locale::parseLanguage(v);
            if (!l) throw std::runtime_error("unknown language " + v);
            lang = *l;
        } else if (a == "-h" || a == "--help") { usage(); return 0; }
        else if (!a.empty() && a[0] != '-' && file.empty()) file = a;
        else { usage(); return 2; }
    }
    if (file.empty()) { usage(); return 2; }

    if (info) {
        if (fs::is_directory(file)) {
            for (const auto& e : fs::directory_iterator(file))
                if (e.path().extension() == ".bik") printInfo(e.path());
        } else {
            printInfo(file);
        }
        return 0;
    }

    video::PlayOptions opt;
    opt.maxFrames = frames;
    opt.screenshot = screenshot;
    opt.font = font;
    opt.audioTrackId = noAudio ? -1 : track != -2 ? track : static_cast<int>(lang);
    if (!noSubs) {
        if (gameDir.empty()) gameDir = std::getenv("OU_GAME_DIR") ? fs::path(std::getenv("OU_GAME_DIR"))
                                                                    : fs::absolute(file).parent_path().parent_path();
        if (fs::exists(gameDir / "bigfile.000")) {
            auto sch = cdc::BigfileSet(gameDir).read(locale::movieSchemaPath(file.stem().string()));
            if (sch) opt.subtitles = locale::parseMovieSchema({reinterpret_cast<const char*>(sch->data()), sch->size()}, lang);
        }
        std::fprintf(stderr, "subtitles: %zu cue(s) for %s\n", opt.subtitles.size(),
                     std::string(locale::languageName(lang)).c_str());
    }

    platform::SdlInit sdl(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
    if (!sdl.ok) { std::fprintf(stderr, "ou-movie: no display: %s\n", SDL_GetError()); return 77; }
    if (!noAudio && !SDL_InitSubSystem(SDL_INIT_AUDIO))
        std::fprintf(stderr, "ou-movie: no audio (%s); using the wall clock\n", SDL_GetError());
    const video::MovieInfo mi = video::Movie(file, -1).info();
    platform::WindowPtr window(SDL_CreateWindow(("ou-movie - " + file.filename().string()).c_str(), mi.width, mi.height,
                                                SDL_WINDOW_RESIZABLE));
    if (!window) { std::fprintf(stderr, "ou-movie: no window: %s\n", SDL_GetError()); return 77; }

    const video::PlayStats s = video::playMovie(window.get(), file, opt);
    if (stats || frames > 0)
        std::printf("frames %d (dropped %d), t %.3f s, %s, skipped %d, drift ms: mean|.| %.2f max|.| %.2f "
                    "first-second %.2f last-second %.2f\n",
                    s.frames, s.dropped, s.seconds, s.audio ? "audio clock" : "wall clock", int(s.skipped),
                    s.meanAbsDriftMs, s.maxAbsDriftMs, s.firstSecondDriftMs, s.lastSecondDriftMs);
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "ou-movie: %s\n", e.what());
    return 1;
}
