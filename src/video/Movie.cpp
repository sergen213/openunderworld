#include "video/Movie.hpp"

#include <stdexcept>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace ou::video {

namespace {

[[noreturn]] void fail(const std::string& what, int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof buf);
    throw std::runtime_error(what + ": " + buf);
}

AVCodecContext* openDecoder(const AVStream* st) {
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) throw std::runtime_error(std::string("no decoder for ") + avcodec_get_name(st->codecpar->codec_id));
    AVCodecContext* c = avcodec_alloc_context3(codec);
    int r = avcodec_parameters_to_context(c, st->codecpar);
    if (r >= 0) r = avcodec_open2(c, codec, nullptr);
    if (r < 0) {
        avcodec_free_context(&c);
        fail("open decoder", r);
    }
    return c;
}

} // namespace

struct Movie::Impl {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* vdec = nullptr;
    AVCodecContext* adec = nullptr;
    SwsContext* sws = nullptr;
    SwrContext* swr = nullptr;
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frm = av_frame_alloc();
    int vstream = -1;
    int track = -1; // index into info.audio
    bool demuxEof = false;
    int64_t frames = 0;
    MovieInfo info;

    ~Impl() {
        swr_free(&swr);
        sws_freeContext(sws);
        avcodec_free_context(&adec);
        avcodec_free_context(&vdec);
        avformat_close_input(&fmt);
        av_frame_free(&frm);
        av_packet_free(&pkt);
    }

    void drainAudio(std::vector<float>& out) {
        while (avcodec_receive_frame(adec, frm) == 0) {
            if (!swr) {
                AVChannelLayout layout;
                av_channel_layout_default(&layout, adec->ch_layout.nb_channels);
                int r = swr_alloc_set_opts2(&swr, &layout, AV_SAMPLE_FMT_FLT, adec->sample_rate, &layout,
                                            AVSampleFormat(frm->format), adec->sample_rate, 0, nullptr);
                if (r >= 0) r = swr_init(swr);
                if (r < 0) fail("swresample", r);
            }
            const int ch = adec->ch_layout.nb_channels;
            const size_t base = out.size();
            out.resize(base + size_t(swr_get_out_samples(swr, frm->nb_samples)) * ch);
            uint8_t* dst = reinterpret_cast<uint8_t*>(out.data() + base);
            const int n = swr_convert(swr, &dst, int((out.size() - base) / ch),
                                      const_cast<const uint8_t**>(frm->extended_data), frm->nb_samples);
            out.resize(base + size_t(n < 0 ? 0 : n) * ch);
            av_frame_unref(frm);
        }
    }
};

Movie::Movie(const std::filesystem::path& file, int audioTrackId) : d_(std::make_unique<Impl>()) {
    Impl& d = *d_;
    int r = avformat_open_input(&d.fmt, file.c_str(), nullptr, nullptr);
    if (r < 0) fail(file.string(), r);
    if ((r = avformat_find_stream_info(d.fmt, nullptr)) < 0) fail("stream info", r);
    d.vstream = av_find_best_stream(d.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (d.vstream < 0) throw std::runtime_error(file.string() + ": no video stream");

    const AVStream* vs = d.fmt->streams[d.vstream];
    d.info.format = d.fmt->iformat->name;
    d.info.videoCodec = avcodec_get_name(vs->codecpar->codec_id);
    d.info.width = vs->codecpar->width;
    d.info.height = vs->codecpar->height;
    d.info.fps = av_q2d(vs->avg_frame_rate.num ? vs->avg_frame_rate : vs->r_frame_rate);
    d.info.duration = d.fmt->duration > 0 ? double(d.fmt->duration) / AV_TIME_BASE : 0;
    for (unsigned i = 0; i < d.fmt->nb_streams; ++i) {
        AVStream* st = d.fmt->streams[i];
        if (int(i) != d.vstream) st->discard = AVDISCARD_ALL;
        if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
        d.info.audio.push_back({int(i), st->id, st->codecpar->sample_rate, st->codecpar->ch_layout.nb_channels,
                                avcodec_get_name(st->codecpar->codec_id)});
        if (st->id == audioTrackId) d.track = int(d.info.audio.size()) - 1;
    }
    if (audioTrackId >= 0 && d.track < 0 && !d.info.audio.empty()) d.track = 0;

    d.vdec = openDecoder(vs);
    if (d.track >= 0) {
        AVStream* as = d.fmt->streams[d.info.audio[d.track].stream];
        as->discard = AVDISCARD_DEFAULT;
        d.adec = openDecoder(as);
    }
}

Movie::~Movie() = default;

const MovieInfo& Movie::info() const { return d_->info; }
const AudioTrack* Movie::audioTrack() const { return d_->track >= 0 ? &d_->info.audio[d_->track] : nullptr; }

bool Movie::next(VideoFrame& frame, std::vector<float>& audio) {
    Impl& d = *d_;
    for (;;) {
        const int r = avcodec_receive_frame(d.vdec, d.frm);
        if (r == 0) break;
        if (r != AVERROR(EAGAIN)) return false; // EOF after flush, or a decoder error
        if (av_read_frame(d.fmt, d.pkt) < 0) {
            if (d.demuxEof) return false;
            d.demuxEof = true;
            avcodec_send_packet(d.vdec, nullptr);
            if (d.adec) {
                avcodec_send_packet(d.adec, nullptr);
                d.drainAudio(audio);
            }
            continue;
        }
        if (d.pkt->stream_index == d.vstream) {
            avcodec_send_packet(d.vdec, d.pkt); // a corrupt packet only costs its frame
        } else if (d.adec && d.pkt->stream_index == d.info.audio[d.track].stream) {
            avcodec_send_packet(d.adec, d.pkt);
            d.drainAudio(audio);
        }
        av_packet_unref(d.pkt);
    }

    const int w = d.info.width, h = d.info.height;
    d.sws = sws_getCachedContext(d.sws, d.frm->width, d.frm->height, AVPixelFormat(d.frm->format), w, h,
                                 AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!d.sws) throw std::runtime_error("swscale: unsupported frame format");
    frame.rgba.resize(size_t(w) * h * 4);
    uint8_t* dst[1] = {frame.rgba.data()};
    const int stride[1] = {w * 4};
    sws_scale(d.sws, d.frm->data, d.frm->linesize, 0, d.frm->height, dst, stride);
    const int64_t ts = d.frm->best_effort_timestamp;
    frame.index = d.frames++;
    frame.pts = ts == AV_NOPTS_VALUE ? frame.index / d.info.fps
                                     : ts * av_q2d(d.fmt->streams[d.vstream]->time_base);
    av_frame_unref(d.frm);
    return true;
}

std::vector<float> Movie::decodeAudio() {
    Impl& d = *d_;
    std::vector<float> out;
    if (!d.adec) return out;
    d.fmt->streams[d.vstream]->discard = AVDISCARD_ALL;
    const int stream = d.info.audio[d.track].stream;
    while (av_read_frame(d.fmt, d.pkt) >= 0) {
        if (d.pkt->stream_index == stream) avcodec_send_packet(d.adec, d.pkt), d.drainAudio(out);
        av_packet_unref(d.pkt);
    }
    avcodec_send_packet(d.adec, nullptr);
    d.drainAudio(out);
    return out;
}

} // namespace ou::video
