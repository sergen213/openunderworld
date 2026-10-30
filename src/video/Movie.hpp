// Bink movie decoding through libavformat/libavcodec (binkvideo + binkaudio_dct), RGBA via
// swscale, interleaved float audio via swresample. Independent of SDL and the game. docs/formats/VIDEO.md.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace ou::video {

struct AudioTrack {
    int stream = -1;     // libav stream index
    int id = 0;          // Bink track id (tru.exe requests its language index, 0x4A0C70)
    int sampleRate = 0;
    int channels = 0;
    std::string codec;
};

struct MovieInfo {
    std::string format, videoCodec;
    int width = 0, height = 0;
    double fps = 0, duration = 0; // seconds
    std::vector<AudioTrack> audio;
};

struct VideoFrame {
    int64_t index = 0;       // decode order, from 0
    double pts = 0;          // seconds
    std::vector<uint8_t> rgba; // width * height * 4, top row first
};

class Movie {
public:
    // Plays the audio track with Bink id `audioTrackId` and falls back to the first track when no
    // track has that id (as tru.exe 0x4A0C70). -1 = decode no audio. Throws std::runtime_error.
    explicit Movie(const std::filesystem::path& file, int audioTrackId = 0);
    ~Movie();
    Movie(const Movie&) = delete;
    Movie& operator=(const Movie&) = delete;

    const MovieInfo& info() const;
    const AudioTrack* audioTrack() const; // selected track, nullptr = none

    // Decodes up to and including the next video frame. Audio of the selected track met on the way is
    // appended to `audio` as interleaved float32 (audioTrack()->channels per sample frame).
    // Returns false at the end of the movie (after flushing the decoders).
    bool next(VideoFrame& frame, std::vector<float>& audio);
    // The whole selected audio track (interleaved float32), without decoding video. Call instead of next().
    std::vector<float> decodeAudio();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

} // namespace ou::video
