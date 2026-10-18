#include "video/Player.hpp"

#include <array>
#include <cmath>
#include <deque>
#include <fstream>
#include <stdexcept>

#include <SDL3/SDL.h>
#ifdef OU_HAVE_TTF
#include <SDL3_ttf/SDL_ttf.h>
#endif

#include "video/Movie.hpp"

namespace ou::video {

namespace {

// Audio clock = sample frames handed to SDL minus those still queued in the stream, minus one
// device buffer of output latency.
struct AudioOut {
    SDL_AudioStream* stream = nullptr;
    int channels = 0, rate = 0;
    int64_t pushed = 0; // sample frames
    double latency = 0;

    AudioOut(const AudioTrack* t) {
        if (!t || !(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO)) return;
        const SDL_AudioSpec spec{SDL_AUDIO_F32, t->channels, t->sampleRate};
        stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!stream) return;
        channels = t->channels;
        rate = t->sampleRate;
        SDL_AudioSpec dev;
        int frames = 0;
        if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream), &dev, &frames) && dev.freq > 0)
            latency = double(frames) / dev.freq;
    }
    ~AudioOut() { if (stream) SDL_DestroyAudioStream(stream); }
    AudioOut(const AudioOut&) = delete;
    AudioOut& operator=(const AudioOut&) = delete;

    int64_t queuedFrames() const { return SDL_GetAudioStreamQueued(stream) / (4 * channels); }
    double clock() const { return double(pushed - queuedFrames()) / rate - latency; }
    double queuedSeconds() const { return double(queuedFrames()) / rate; }
    void push(std::vector<float>& samples) {
        if (stream && !samples.empty()) {
            SDL_PutAudioStreamData(stream, samples.data(), int(samples.size() * sizeof(float)));
            pushed += int64_t(samples.size()) / channels;
        }
        samples.clear();
    }
};

void writePpm(SDL_Renderer* r, const std::filesystem::path& path) {
    SDL_Surface* raw = SDL_RenderReadPixels(r, nullptr);
    SDL_Surface* rgb = raw ? SDL_ConvertSurface(raw, SDL_PIXELFORMAT_RGB24) : nullptr;
    SDL_DestroySurface(raw);
    if (!rgb) throw std::runtime_error(std::string("screenshot: ") + SDL_GetError());
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << rgb->w << ' ' << rgb->h << "\n255\n";
    for (int y = 0; y < rgb->h; ++y)
        f.write(static_cast<const char*>(rgb->pixels) + size_t(y) * rgb->pitch, std::streamsize(rgb->w) * 3);
    SDL_DestroySurface(rgb);
    if (!f) throw std::runtime_error("screenshot: cannot write " + path.string());
}

// Subtitle lines at tru.exe's place: a 640x480 virtual screen, y 352 and 374 (0x583E20), centred.
class SubtitleDrawer {
public:
    SubtitleDrawer(SDL_Renderer* r, int height, const std::filesystem::path& fontPath) : r_(r), h_(height) {
#ifdef OU_HAVE_TTF
        static const std::array<const char*, 3> kFonts = {"/usr/share/fonts/noto/NotoSans-Regular.ttf",
                                                          "/usr/share/fonts/TTF/DejaVuSans.ttf",
                                                          "/usr/share/fonts/liberation/LiberationSans-Regular.ttf"};
        if (!TTF_Init()) return;
        for (const char* p : kFonts) {
            const std::string path = fontPath.empty() ? p : fontPath.string();
            if ((font_ = TTF_OpenFont(path.c_str(), float(height) * 0.04f))) break;
            if (!fontPath.empty()) break;
        }
        if (font_) engine_ = TTF_CreateRendererTextEngine(r);
#else
        (void)fontPath;
#endif
    }
    ~SubtitleDrawer() {
#ifdef OU_HAVE_TTF
        if (engine_) TTF_DestroyRendererTextEngine(engine_);
        if (font_) TTF_CloseFont(font_);
        TTF_Quit();
#endif
    }
    SubtitleDrawer(const SubtitleDrawer&) = delete;
    SubtitleDrawer& operator=(const SubtitleDrawer&) = delete;

    // Logical (movie) coordinates; centred at `width`/2.
    void draw(const std::string& line, int slot, int width) {
        if (line.empty()) return;
        const float y = (352.0f + 22.0f * slot) / 480.0f * h_;
#ifdef OU_HAVE_TTF
        if (engine_) {
            TTF_Text* t = TTF_CreateText(engine_, font_, line.c_str(), 0);
            int tw = 0, th = 0;
            TTF_GetTextSize(t, &tw, &th);
            const float x = (width - tw) / 2.0f;
            TTF_SetTextColor(t, 0, 0, 0, 255);
            TTF_DrawRendererText(t, x + 2, y + 2);
            TTF_SetTextColor(t, 255, 255, 255, 255);
            TTF_DrawRendererText(t, x, y);
            TTF_DestroyText(t);
            return;
        }
#endif
        // ponytail: SDL debug font (ASCII only, other bytes show as '?') when SDL3_ttf or a font is missing.
        const float s = std::max(1.0f, h_ / 240.0f);
        SDL_SetRenderScale(r_, s, s);
        const float x = (width / s - 8.0f * float(line.size())) / 2.0f;
        SDL_SetRenderDrawColor(r_, 0, 0, 0, 255);
        SDL_RenderDebugText(r_, x + 1 / s, y / s + 1 / s, line.c_str());
        SDL_SetRenderDrawColor(r_, 255, 255, 255, 255);
        SDL_RenderDebugText(r_, x, y / s, line.c_str());
        SDL_SetRenderScale(r_, 1, 1);
    }

private:
    SDL_Renderer* r_;
    float h_;
#ifdef OU_HAVE_TTF
    TTF_Font* font_ = nullptr;
    TTF_TextEngine* engine_ = nullptr;
#endif
};

struct RendererDeleter { void operator()(SDL_Renderer* r) const { SDL_DestroyRenderer(r); } };
struct TextureDeleter { void operator()(SDL_Texture* t) const { SDL_DestroyTexture(t); } };

} // namespace

PlayStats playMovie(SDL_Window* window, const std::filesystem::path& file, const PlayOptions& options) {
    Movie movie(file, options.audioTrackId);
    const MovieInfo& info = movie.info();

    std::unique_ptr<SDL_Renderer, RendererDeleter> renderer(SDL_CreateRenderer(window, nullptr));
    if (!renderer) throw std::runtime_error(std::string("SDL_CreateRenderer: ") + SDL_GetError());
    SDL_Renderer* r = renderer.get();
    SDL_SetRenderVSync(r, 1);
    SDL_SetRenderLogicalPresentation(r, info.width, info.height, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    std::unique_ptr<SDL_Texture, TextureDeleter> texture(
        SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, info.width, info.height));
    if (!texture) throw std::runtime_error(std::string("SDL_CreateTexture: ") + SDL_GetError());

    SubtitleDrawer drawer(r, info.height, options.font);
    locale::MovieSubtitles subtitles(options.subtitles);
    AudioOut audio(movie.audioTrack());

    std::deque<VideoFrame> queue;
    std::vector<float> samples;
    bool eof = false;
    auto refill = [&] {
        while (!eof && (queue.size() < 2 || (audio.stream && audio.queuedSeconds() < 0.25 && queue.size() < 16))) {
            VideoFrame f;
            if (movie.next(f, samples)) queue.push_back(std::move(f));
            else eof = true;
            audio.push(samples);
        }
    };

    // Master clock: audio while it plays, then wall time continuing from where the audio stopped.
    bool audioClock = audio.stream != nullptr;
    double wallBaseClock = 0;
    uint64_t wallBase = SDL_GetTicksNS();
    auto clock = [&] {
        if (audioClock) {
            if (!(eof && audio.queuedFrames() == 0)) return audio.clock();
            audioClock = false;
            wallBaseClock = audio.clock();
            wallBase = SDL_GetTicksNS();
        }
        return wallBaseClock + double(SDL_GetTicksNS() - wallBase) * 1e-9;
    };

    PlayStats stats;
    stats.audio = audio.stream != nullptr;
    std::vector<std::pair<double, double>> drift; // (pts, drift seconds)
    refill();
    if (audio.stream) SDL_ResumeAudioStreamDevice(audio.stream);
    wallBase = SDL_GetTicksNS();
    uint64_t lastTick = wallBase;
    bool have = false;
    double shownPts = 0;
    const double frameTime = info.fps > 0 ? 1.0 / info.fps : 1.0 / 30;

    for (;;) {
        for (SDL_Event e; SDL_PollEvent(&e);) {
            const bool input = e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                               e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED || (input && options.skippable))
                stats.skipped = true;
        }
        if (stats.skipped) break;

        refill();
        const double now = clock();
        bool newFrame = false;
        // First frame at once; later ones when due, dropping frames that are already late.
        while (!queue.empty() && (!have || queue.front().pts <= now)) {
            const bool late = have && queue.size() > 1 && queue[1].pts <= now;
            if (late) ++stats.dropped;
            else {
                SDL_UpdateTexture(texture.get(), nullptr, queue.front().rgba.data(), info.width * 4);
                shownPts = queue.front().pts;
                drift.emplace_back(shownPts, shownPts - now);
                ++stats.frames;
                newFrame = have = true;
            }
            queue.pop_front();
            refill();
            if (newFrame) break;
        }
        if (queue.empty() && eof && !audioClock && now >= shownPts + frameTime) break;

        const uint64_t t = SDL_GetTicksNS();
        subtitles.update(shownPts, double(t - lastTick) * 1e-9 * 30.0); // timeMult: 1 tick = 1/30 s
        lastTick = t;

        SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
        SDL_RenderClear(r);
        SDL_RenderTexture(r, texture.get(), nullptr, nullptr);
        drawer.draw(subtitles.ticker().top(), 0, info.width);
        drawer.draw(subtitles.ticker().bottom(), 1, info.width);
        if (newFrame && options.maxFrames > 0 && stats.frames >= options.maxFrames) {
            if (!options.screenshot.empty()) writePpm(r, options.screenshot);
            break;
        }
        SDL_RenderPresent(r);
    }

    stats.seconds = shownPts;
    double sum = 0, first = 0, last = 0;
    int nFirst = 0, nLast = 0;
    for (auto [pts, d] : drift) {
        sum += std::abs(d);
        stats.maxAbsDriftMs = std::max(stats.maxAbsDriftMs, std::abs(d) * 1000);
        if (pts < 1.0) first += d, ++nFirst;
        if (pts > shownPts - 1.0) last += d, ++nLast;
    }
    if (!drift.empty()) stats.meanAbsDriftMs = sum / double(drift.size()) * 1000;
    if (nFirst) stats.firstSecondDriftMs = first / nFirst * 1000;
    if (nLast) stats.lastSecondDriftMs = last / nLast * 1000;
    return stats;
}

} // namespace ou::video
