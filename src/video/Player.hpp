// Plays a movie in an SDL window through SDL_Renderer (streaming RGBA texture), audio through an
// SDL3 audio stream; video frames are presented against the audio clock. docs/formats/VIDEO.md.
#pragma once

#include <filesystem>
#include <vector>

#include "locale/Locale.hpp"

struct SDL_Window;

namespace ou::video {

struct PlayOptions {
    int audioTrackId = 0;             // Bink track id (= tru.exe language index); -1 = silent
    int maxFrames = -1;               // stop after this many presented frames (-1 = whole movie)
    std::filesystem::path screenshot; // binary PPM of the last presented frame (with subtitles)
    std::vector<locale::Cue> subtitles;
    std::filesystem::path font;       // TTF for subtitles; empty = first found system font
    bool skippable = true;            // any key / mouse button / gamepad button ends playback
};

struct PlayStats {
    int frames = 0;            // presented video frames
    int dropped = 0;           // decoded frames skipped because they were already late
    bool skipped = false;      // ended by input or window close
    bool audio = false;        // played through an audio device (else wall clock)
    double seconds = 0;        // movie time of the last presented frame
    // drift = frame pts - audio clock at presentation, milliseconds
    double meanAbsDriftMs = 0, maxAbsDriftMs = 0, firstSecondDriftMs = 0, lastSecondDriftMs = 0;
};

// Requires SDL_INIT_VIDEO (+ SDL_INIT_AUDIO for sound). The window must not have another renderer or
// a GL/Vulkan context in use; an SDL_Renderer is created on it and destroyed before returning.
// Throws std::runtime_error when the movie cannot be opened or no SDL_Renderer can be created.
PlayStats playMovie(SDL_Window* window, const std::filesystem::path& file, const PlayOptions& options = {});

} // namespace ou::video
