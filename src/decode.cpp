#include "decode.hpp"

#include "util.hpp"
#include "v210.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cstring>

namespace mtp {
namespace {

void fit_rect(ScaleMode mode, int sw, int sh, int bw, int bh, int& dw, int& dh, int& ox, int& oy) {
    dw = bw;
    dh = bh;
    ox = 0;
    oy = 0;
    if (sw <= 0 || sh <= 0 || bw <= 0 || bh <= 0) return;
    if (mode == ScaleMode::SpriteMax) {
        const double s = std::min(1.0, static_cast<double>(bw) / std::max(sw, sh));
        dw = std::max(1, static_cast<int>(sw * s));
        dh = std::max(1, static_cast<int>(sh * s));
        return;
    }
    const double sx = static_cast<double>(bw) / sw;
    const double sy = static_cast<double>(bh) / sh;
    if (mode == ScaleMode::Fill) {
        const double s = std::max(sx, sy);
        dw = std::max(1, static_cast<int>(sw * s));
        dh = std::max(1, static_cast<int>(sh * s));
        ox = (bw - dw) / 2;
        oy = (bh - dh) / 2;
    } else if (mode == ScaleMode::Center) {
        dw = sw;
        dh = sh;
        ox = (bw - dw) / 2;
        oy = (bh - dh) / 2;
    } else {
        const double s = std::min(sx, sy);
        dw = std::max(1, static_cast<int>(sw * s));
        dh = std::max(1, static_cast<int>(sh * s));
        ox = (bw - dw) / 2;
        oy = (bh - dh) / 2;
    }
}

void blit_rgba(std::uint8_t* dst, int dw, int dh, const std::uint8_t* src, int sw, int sh, int ox, int oy) {
    for (int y = 0; y < sh; ++y) {
        const int dy = oy + y;
        if (dy < 0 || dy >= dh) continue;
        const int x0 = std::max(0, -ox);
        const int x1 = std::min(sw, dw - ox);
        if (x0 >= x1) continue;
        std::memcpy(dst + (static_cast<std::size_t>(dy) * dw + (ox + x0)) * 4, src + (static_cast<std::size_t>(y) * sw + x0) * 4,
                    static_cast<std::size_t>(x1 - x0) * 4);
    }
}

bool frame_to_v210(const AVFrame* frame, std::vector<std::uint8_t>& v210, std::string& error) {
    const int w = frame->width;
    const int h = frame->height;
    v210.resize(v210_size(w, h));
    if (frame->format == AV_PIX_FMT_YUV422P10LE) {
        std::vector<std::uint16_t> y(static_cast<std::size_t>(w));
        std::vector<std::uint16_t> cb(static_cast<std::size_t>(w / 2));
        std::vector<std::uint16_t> cr(static_cast<std::size_t>(w / 2));
        const int ystride = frame->linesize[0] / 2;
        const int ustride = frame->linesize[1] / 2;
        const int vstride = frame->linesize[2] / 2;
        const std::uint32_t stride = v210_line_stride(w);
        for (int row = 0; row < h; ++row) {
            const auto* ys = reinterpret_cast<const std::uint16_t*>(frame->data[0]) + static_cast<std::size_t>(row) * ystride;
            const auto* us = reinterpret_cast<const std::uint16_t*>(frame->data[1]) + static_cast<std::size_t>(row) * ustride;
            const auto* vs = reinterpret_cast<const std::uint16_t*>(frame->data[2]) + static_cast<std::size_t>(row) * vstride;
            for (int x = 0; x < w; ++x) y[x] = ys[x] & 0x3FF;
            for (int x = 0; x < w / 2; ++x) {
                cb[x] = us[x] & 0x3FF;
                cr[x] = vs[x] & 0x3FF;
            }
            pack_v210_line(y.data(), cb.data(), cr.data(), w, v210.data() + static_cast<std::size_t>(row) * stride);
        }
        return true;
    }
    SwsContext* sws = sws_getContext(w, h, static_cast<AVPixelFormat>(frame->format), w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr,
                                     nullptr, nullptr);
    if (!sws) {
        error = "sws failed";
        return false;
    }
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4);
    uint8_t* dst_data[4] = {rgba.data(), nullptr, nullptr, nullptr};
    int dst_lines[4] = {w * 4, 0, 0, 0};
    sws_scale(sws, frame->data, frame->linesize, 0, h, dst_data, dst_lines);
    sws_freeContext(sws);
    rgba_to_fill_key(rgba.data(), w, h, false, v210.data(), nullptr, nullptr, kYBlack, kYWhite);
    return true;
}

}  // namespace

void rgba_to_fill_key(const std::uint8_t* rgba, int w, int h, bool premultiply, std::uint8_t* fill_v210, std::uint8_t* key_v210,
                      std::uint8_t* alpha10, int key_min, int key_max) {
    std::vector<std::uint16_t> y(static_cast<std::size_t>(w));
    std::vector<std::uint16_t> cb(static_cast<std::size_t>(w / 2), kCMid);
    std::vector<std::uint16_t> cr(static_cast<std::size_t>(w / 2), kCMid);
    std::vector<std::uint16_t> ky(static_cast<std::size_t>(w));
    std::vector<std::uint16_t> kcb(static_cast<std::size_t>(w / 2), kCMid);
    std::vector<std::uint16_t> kcr(static_cast<std::size_t>(w / 2), kCMid);
    std::vector<std::uint16_t> a10(static_cast<std::size_t>(w));
    const std::uint32_t stride = v210_line_stride(w);
    const std::uint32_t astride = alpha10_line_stride(w);
    for (int row = 0; row < h; ++row) {
        for (int x = 0; x < w; ++x) {
            const std::uint8_t* p = rgba + (static_cast<std::size_t>(row) * w + x) * 4;
            double r = p[0] / 255.0, g = p[1] / 255.0, b = p[2] / 255.0, a = p[3] / 255.0;
            if (premultiply) {
                r *= a;
                g *= a;
                b *= a;
            }
            const Yuv10 yuv = rgb_to_yuv709(r, g, b);
            y[x] = static_cast<std::uint16_t>(yuv.y);
            const int c = x / 2;
            cb[c] = static_cast<std::uint16_t>(yuv.cb);
            cr[c] = static_cast<std::uint16_t>(yuv.cr);
            const int key = alpha_to_key(a, key_min, key_max);
            ky[x] = static_cast<std::uint16_t>(key);
            a10[x] = static_cast<std::uint16_t>(std::clamp(static_cast<int>(a * 1023.0 + 0.5), 0, 1023));
        }
        if (fill_v210) pack_v210_line(y.data(), cb.data(), cr.data(), w, fill_v210 + static_cast<std::size_t>(row) * stride);
        if (key_v210) pack_v210_line(ky.data(), kcb.data(), kcr.data(), w, key_v210 + static_cast<std::size_t>(row) * stride);
        if (alpha10) pack_alpha10_line(a10.data(), w, alpha10 + static_cast<std::size_t>(row) * astride);
    }
}

void extract_v210_field(const std::uint8_t* frame, int width, int height, int lines, std::uint8_t* dst) {
    const std::uint32_t stride = v210_line_stride(width);
    int out = 0;
    for (int y = lines & 1; y < height; y += 2) {
        std::memcpy(dst + static_cast<std::size_t>(out) * stride, frame + static_cast<std::size_t>(y) * stride, stride);
        ++out;
    }
}

bool write_jpeg(const std::string& path, const std::uint8_t* rgb, int w, int h, int quality) {
    return stbi_write_jpg(path.c_str(), w, h, 3, rgb, quality) != 0;
}

bool v210_to_jpeg(const std::string& path, const std::uint8_t* v210, int w, int h, int out_w) {
    if (w <= 0 || h <= 0 || !v210) return false;
    if (out_w <= 0) out_w = std::min(w, 320);
    const int out_h = std::max(1, h * out_w / w);
    std::vector<std::uint8_t> rgb(static_cast<std::size_t>(out_w) * out_h * 3);
    std::vector<std::uint16_t> y(static_cast<std::size_t>(w));
    std::vector<std::uint16_t> cb(static_cast<std::size_t>(w / 2));
    std::vector<std::uint16_t> cr(static_cast<std::size_t>(w / 2));
    const std::uint32_t stride = v210_line_stride(w);
    for (int oy = 0; oy < out_h; ++oy) {
        const int sy = std::min(h - 1, oy * h / out_h);
        unpack_v210_line(v210 + static_cast<std::size_t>(sy) * stride, w, y.data(), cb.data(), cr.data());
        for (int ox = 0; ox < out_w; ++ox) {
            const int sx = std::min(w - 1, ox * w / out_w);
            const double Y = (y[sx] - 64) / 876.0;
            const double Cb = (cb[sx / 2] - 512) / 896.0;
            const double Cr = (cr[sx / 2] - 512) / 896.0;
            const double r = Y + 1.5748 * Cr;
            const double g = Y - 0.1873 * Cb - 0.4681 * Cr;
            const double b = Y + 1.8556 * Cb;
            auto* d = rgb.data() + (static_cast<std::size_t>(oy) * out_w + ox) * 3;
            auto sat = [](double v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0, 1.0) * 255.0 + 0.5); };
            d[0] = sat(r);
            d[1] = sat(g);
            d[2] = sat(b);
        }
    }
    return write_jpeg(path, rgb.data(), out_w, out_h, 80);
}

namespace {

struct Opened {
    AVFormatContext* fmt = nullptr;
    int vindex = -1;
    ~Opened() {
        if (fmt) avformat_close_input(&fmt);
    }
    bool open(const std::string& path, std::string& error) {
        if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
            error = "open failed";
            return false;
        }
        if (avformat_find_stream_info(fmt, nullptr) < 0) {
            error = "no stream info";
            return false;
        }
        vindex = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vindex < 0) {
            error = "no video stream";
            return false;
        }
        const auto* par = fmt->streams[vindex]->codecpar;
        if (static_cast<std::int64_t>(par->width) * par->height > kMaxImagePixels) {
            error = "image is " + std::to_string(par->width) + "x" + std::to_string(par->height) + ", more pixels than 8192x4608";
            return false;
        }
        return true;
    }
};

// A frame's duration; 0.1 s when it has none. A GIF delay of 10 ms or less is 100 ms, as browsers
// play it (such GIFs are made for that).
double frame_seconds(std::int64_t duration, AVRational tb, bool gif) {
    const double d = duration > 0 ? duration * av_q2d(tb) : 0.0;
    if (d <= 0 || (gif && d <= 0.0105)) return 0.1;
    return d;
}

}  // namespace

std::string probe_video_codec(const std::string& path) {
    Opened in;
    std::string error;
    if (!in.open(path, error)) return {};
    return avcodec_get_name(in.fmt->streams[in.vindex]->codecpar->codec_id);
}

bool probe_visual(const std::string& path, VisualInfo& info, std::string& error) {
    Opened in;
    if (!in.open(path, error)) return false;
    AVStream* st = in.fmt->streams[in.vindex];
    info = {};
    info.w = st->codecpar->width;
    info.h = st->codecpar->height;
    AVPacket* pkt = av_packet_alloc();
    while (av_read_frame(in.fmt, pkt) >= 0) {
        if (pkt->stream_index == in.vindex) {
            ++info.frames;
            info.total_s += frame_seconds(pkt->duration, st->time_base, st->codecpar->codec_id == AV_CODEC_ID_GIF);
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    if (info.frames <= 0) {
        error = "no frames";
        return false;
    }
    return true;
}

bool decode_visual_frames(const std::string& path, const std::function<bool(const RgbaBuffer&, double)>& on_frame, std::string& error) {
    Opened in;
    if (!in.open(path, error)) return false;
    AVStream* st = in.fmt->streams[in.vindex];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext* dec = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (!dec || avcodec_parameters_to_context(dec, st->codecpar) < 0 || avcodec_open2(dec, codec, nullptr) < 0) {
        avcodec_free_context(&dec);
        error = "decoder open failed";
        return false;
    }
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    SwsContext* sws = nullptr;
    RgbaBuffer buf;
    int count = 0;
    bool ok = true;
    bool more = true;
    auto take = [&]() {
        while (ok && more && avcodec_receive_frame(dec, frame) == 0) {
            if (++count > kMaxAnimationFrames) {
                error = "animation has more than " + std::to_string(kMaxAnimationFrames) + " frames";
                ok = false;
            } else {
                sws = sws_getCachedContext(sws, frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), frame->width,
                                           frame->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (!sws) {
                    error = "scale failed";
                    ok = false;
                } else {
                    buf.w = frame->width;
                    buf.h = frame->height;
                    buf.px.resize(static_cast<std::size_t>(buf.w) * buf.h * 4);
                    uint8_t* dst[4] = {buf.px.data(), nullptr, nullptr, nullptr};
                    int lines[4] = {buf.w * 4, 0, 0, 0};
                    sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, lines);
                    more = on_frame(buf, frame_seconds(frame->duration, st->time_base, st->codecpar->codec_id == AV_CODEC_ID_GIF));
                }
            }
            av_frame_unref(frame);
        }
    };
    while (ok && more && av_read_frame(in.fmt, pkt) >= 0) {
        if (pkt->stream_index == in.vindex && avcodec_send_packet(dec, pkt) >= 0) take();
        av_packet_unref(pkt);
    }
    if (ok && more) {
        avcodec_send_packet(dec, nullptr);
        take();
    }
    if (sws) sws_freeContext(sws);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&dec);
    if (ok && count == 0) {
        error = "no frames decoded";
        ok = false;
    }
    return ok;
}

RgbaFitter::~RgbaFitter() {
    if (sws_) sws_freeContext(static_cast<SwsContext*>(sws_));
}

void RgbaFitter::fit(const RgbaBuffer& src, ScaleMode mode, int box_w, int box_h, RgbaBuffer& out) {
    int dw = 0, dh = 0, ox = 0, oy = 0;
    fit_rect(mode, src.w, src.h, box_w, box_h, dw, dh, ox, oy);
    if (mode == ScaleMode::SpriteMax) {
        box_w = dw;
        box_h = dh;
        ox = oy = 0;
    }
    auto* sws = sws_getCachedContext(static_cast<SwsContext*>(sws_), src.w, src.h, AV_PIX_FMT_RGBA, dw, dh, AV_PIX_FMT_RGBA, SWS_BILINEAR,
                                     nullptr, nullptr, nullptr);
    sws_ = sws;
    out.w = box_w;
    out.h = box_h;
    out.px.assign(static_cast<std::size_t>(box_w) * box_h * 4, 0);
    if (!sws) return;
    // Scaled premultiplied, then straight again: the colour of transparent pixels (often white in
    // a GIF) does not bleed into the edges, and the fill is black where the alpha is 0.
    premul_.resize(src.px.size());
    for (std::size_t i = 0; i + 3 < src.px.size(); i += 4) {
        const unsigned a = src.px[i + 3];
        for (int c = 0; c < 3; ++c) premul_[i + c] = static_cast<std::uint8_t>((src.px[i + c] * a + 127) / 255);
        premul_[i + 3] = static_cast<std::uint8_t>(a);
    }
    scaled_.resize(static_cast<std::size_t>(dw) * dh * 4);
    const uint8_t* sdata[4] = {premul_.data(), nullptr, nullptr, nullptr};
    int slines[4] = {src.w * 4, 0, 0, 0};
    uint8_t* ddata[4] = {scaled_.data(), nullptr, nullptr, nullptr};
    int dlines[4] = {dw * 4, 0, 0, 0};
    sws_scale(sws, sdata, slines, 0, src.h, ddata, dlines);
    for (std::size_t i = 0; i + 3 < scaled_.size(); i += 4) {
        const unsigned a = scaled_[i + 3];
        for (int c = 0; c < 3; ++c) scaled_[i + c] = a ? static_cast<std::uint8_t>(std::min(255u, (scaled_[i + c] * 255u + a / 2) / a)) : 0;
    }
    blit_rgba(out.px.data(), box_w, box_h, scaled_.data(), dw, dh, ox, oy);
}

struct MezzanineReader::Impl {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* vdec = nullptr;
    AVCodecContext* adec = nullptr;
    SwrContext* swr = nullptr;
    int vindex = -1;
    int aindex = -1;
    std::int64_t next_frame = 0;
    std::vector<float> audio;
    int channels = 0;
    int width = 0;
    int height = 0;
    std::int64_t frames = 0;
    AVRational rate{25, 1};
};

MezzanineReader::MezzanineReader() = default;
MezzanineReader::~MezzanineReader() { close(); }

void MezzanineReader::close() {
    if (!impl_) return;
    if (impl_->swr) swr_free(&impl_->swr);
    if (impl_->vdec) avcodec_free_context(&impl_->vdec);
    if (impl_->adec) avcodec_free_context(&impl_->adec);
    if (impl_->fmt) avformat_close_input(&impl_->fmt);
    delete impl_;
    impl_ = nullptr;
}

bool MezzanineReader::open(const std::string& path, std::string& error) {
    close();
    impl_ = new Impl();
    if (avformat_open_input(&impl_->fmt, path.c_str(), nullptr, nullptr) < 0) {
        error = "open mezzanine failed";
        close();
        return false;
    }
    avformat_find_stream_info(impl_->fmt, nullptr);
    impl_->vindex = av_find_best_stream(impl_->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    impl_->aindex = av_find_best_stream(impl_->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (impl_->vindex < 0) {
        error = "mezzanine has no video";
        close();
        return false;
    }
    AVStream* vs = impl_->fmt->streams[impl_->vindex];
    const AVCodec* vc = avcodec_find_decoder(vs->codecpar->codec_id);
    impl_->vdec = avcodec_alloc_context3(vc);
    avcodec_parameters_to_context(impl_->vdec, vs->codecpar);
    if (avcodec_open2(impl_->vdec, vc, nullptr) < 0) {
        error = "video decoder failed";
        close();
        return false;
    }
    width_ = impl_->width = vs->codecpar->width;
    height_ = impl_->height = vs->codecpar->height;
    impl_->rate = vs->avg_frame_rate.num ? vs->avg_frame_rate : vs->r_frame_rate;
    if (vs->nb_frames > 0) frames_ = impl_->frames = vs->nb_frames;
    else if (impl_->rate.num && vs->duration > 0) {
        frames_ = impl_->frames = av_rescale_q(vs->duration, vs->time_base, av_inv_q(impl_->rate));
    }
    if (impl_->aindex >= 0) {
        AVStream* as = impl_->fmt->streams[impl_->aindex];
        const AVCodec* ac = avcodec_find_decoder(as->codecpar->codec_id);
        impl_->adec = avcodec_alloc_context3(ac);
        avcodec_parameters_to_context(impl_->adec, as->codecpar);
        if (avcodec_open2(impl_->adec, ac, nullptr) < 0) {
            error = "audio decoder failed";
            close();
            return false;
        }
        channels_ = impl_->channels = as->codecpar->ch_layout.nb_channels;
        AVChannelLayout out_layout;
        av_channel_layout_default(&out_layout, channels_);
        if (swr_alloc_set_opts2(&impl_->swr, &out_layout, AV_SAMPLE_FMT_FLT, 48000, &impl_->adec->ch_layout, impl_->adec->sample_fmt,
                                impl_->adec->sample_rate, 0, nullptr) < 0) {
            error = "swr failed";
            close();
            return false;
        }
        swr_init(impl_->swr);
        av_channel_layout_uninit(&out_layout);
    }
    return true;
}

bool load_mezzanine(const std::string& path, RamClip& out, std::string& error) {
    MezzanineReader reader;
    if (!reader.open(path, error)) return false;
    // Decode sequentially by pulling packets ourselves via a second open for simplicity:
    // reuse reader.read only after we know the length. If frame count is unknown, decode all.
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
        error = "reopen failed";
        return false;
    }
    avformat_find_stream_info(fmt, nullptr);
    const int vindex = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int aindex = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    AVStream* vs = fmt->streams[vindex];
    const AVCodec* vc = avcodec_find_decoder(vs->codecpar->codec_id);
    AVCodecContext* vdec = avcodec_alloc_context3(vc);
    avcodec_parameters_to_context(vdec, vs->codecpar);
    avcodec_open2(vdec, vc, nullptr);
    AVCodecContext* adec = nullptr;
    SwrContext* swr = nullptr;
    int channels = 0;
    if (aindex >= 0) {
        AVStream* as = fmt->streams[aindex];
        const AVCodec* ac = avcodec_find_decoder(as->codecpar->codec_id);
        adec = avcodec_alloc_context3(ac);
        avcodec_parameters_to_context(adec, as->codecpar);
        avcodec_open2(adec, ac, nullptr);
        channels = as->codecpar->ch_layout.nb_channels;
        AVChannelLayout out_layout;
        av_channel_layout_default(&out_layout, channels);
        swr_alloc_set_opts2(&swr, &out_layout, AV_SAMPLE_FMT_FLT, 48000, &adec->ch_layout, adec->sample_fmt, adec->sample_rate, 0, nullptr);
        swr_init(swr);
        av_channel_layout_uninit(&out_layout);
    }
    out.width = vs->codecpar->width;
    out.height = vs->codecpar->height;
    out.channels = channels;
    const std::size_t frame_bytes = v210_size(out.width, out.height);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    auto drain_video = [&](AVFrame* f) {
        std::vector<std::uint8_t> one;
        std::string err;
        if (!frame_to_v210(f, one, err)) return;
        out.v210.insert(out.v210.end(), one.begin(), one.end());
        ++out.frames;
    };
    auto drain_audio = [&](AVFrame* f) {
        if (!swr) return;
        const int dst_nb = swr_get_out_samples(swr, f->nb_samples) + 64;
        std::vector<float> tmp(static_cast<std::size_t>(dst_nb) * channels);
        uint8_t* out_planes[1] = {reinterpret_cast<uint8_t*>(tmp.data())};
        const int got = swr_convert(swr, out_planes, dst_nb, const_cast<const uint8_t**>(f->extended_data), f->nb_samples);
        if (got > 0) {
            out.audio.insert(out.audio.end(), tmp.begin(), tmp.begin() + static_cast<std::size_t>(got) * channels);
            out.audio_samples += static_cast<std::uint64_t>(got);
        }
    };
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == vindex) {
            avcodec_send_packet(vdec, pkt);
            while (avcodec_receive_frame(vdec, frame) == 0) {
                drain_video(frame);
                av_frame_unref(frame);
            }
        } else if (pkt->stream_index == aindex && adec) {
            avcodec_send_packet(adec, pkt);
            while (avcodec_receive_frame(adec, frame) == 0) {
                drain_audio(frame);
                av_frame_unref(frame);
            }
        }
        av_packet_unref(pkt);
    }
    avcodec_send_packet(vdec, nullptr);
    while (avcodec_receive_frame(vdec, frame) == 0) {
        drain_video(frame);
        av_frame_unref(frame);
    }
    if (adec) {
        avcodec_send_packet(adec, nullptr);
        while (avcodec_receive_frame(adec, frame) == 0) {
            drain_audio(frame);
            av_frame_unref(frame);
        }
    }
    (void)frame_bytes;
    av_frame_free(&frame);
    av_packet_free(&pkt);
    if (swr) swr_free(&swr);
    if (adec) avcodec_free_context(&adec);
    avcodec_free_context(&vdec);
    avformat_close_input(&fmt);
    if (out.frames <= 0) {
        error = "mezzanine decode produced no frames";
        return false;
    }
    return true;
}

bool MezzanineReader::read(std::int64_t frame_index, std::uint8_t* v210, float* audio, std::uint64_t audio_start, int audio_count,
                           std::string& error) {
    if (!impl_) {
        error = "reader closed";
        return false;
    }
    // Streaming reads decode forward. A backwards seek reopens by flushing and seeking.
    if (frame_index != impl_->next_frame) {
        const int64_t ts = av_rescale_q(frame_index, av_inv_q(impl_->rate), impl_->fmt->streams[impl_->vindex]->time_base);
        av_seek_frame(impl_->fmt, impl_->vindex, ts, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(impl_->vdec);
        if (impl_->adec) avcodec_flush_buffers(impl_->adec);
        impl_->next_frame = -1;
        impl_->audio.clear();
    }
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    bool got = false;
    std::vector<std::uint8_t> one;
    while (!got && av_read_frame(impl_->fmt, pkt) >= 0) {
        if (pkt->stream_index == impl_->vindex) {
            avcodec_send_packet(impl_->vdec, pkt);
            while (avcodec_receive_frame(impl_->vdec, frame) == 0) {
                std::int64_t idx = impl_->next_frame < 0 ? frame_index : impl_->next_frame;
                if (frame->pts != AV_NOPTS_VALUE) {
                    idx = av_rescale_q(frame->pts, impl_->fmt->streams[impl_->vindex]->time_base, av_inv_q(impl_->rate));
                }
                if (idx >= frame_index) {
                    frame_to_v210(frame, one, error);
                    got = true;
                    impl_->next_frame = idx + 1;
                }
                av_frame_unref(frame);
                if (got) break;
            }
        } else if (pkt->stream_index == impl_->aindex && impl_->adec) {
            avcodec_send_packet(impl_->adec, pkt);
            while (avcodec_receive_frame(impl_->adec, frame) == 0) {
                const int dst_nb = swr_get_out_samples(impl_->swr, frame->nb_samples) + 32;
                std::vector<float> tmp(static_cast<std::size_t>(dst_nb) * impl_->channels);
                uint8_t* planes[1] = {reinterpret_cast<uint8_t*>(tmp.data())};
                const int n = swr_convert(impl_->swr, planes, dst_nb, const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
                if (n > 0) impl_->audio.insert(impl_->audio.end(), tmp.begin(), tmp.begin() + static_cast<std::size_t>(n) * impl_->channels);
                av_frame_unref(frame);
            }
        }
        av_packet_unref(pkt);
    }
    av_frame_free(&frame);
    av_packet_free(&pkt);
    if (!got || one.empty()) {
        error = "frame not available";
        return false;
    }
    std::memcpy(v210, one.data(), one.size());
    if (audio && audio_count > 0 && impl_->channels > 0) {
        std::fill(audio, audio + static_cast<std::size_t>(audio_count) * impl_->channels, 0.f);
        // audio buffer is a running concatenation; we don't know its absolute start after a seek.
        // Copy what we have from the front when the caller asks for the start of this grain and
        // the buffer is at least that long. The RAM path is preferred for gapless loops.
        const std::uint64_t have = impl_->audio.size() / static_cast<std::size_t>(impl_->channels);
        const std::uint64_t copy_n = std::min<std::uint64_t>(audio_count, have);
        if (copy_n) {
            std::memcpy(audio, impl_->audio.data(), copy_n * impl_->channels * sizeof(float));
            impl_->audio.erase(impl_->audio.begin(), impl_->audio.begin() + static_cast<std::ptrdiff_t>(copy_n * impl_->channels));
        }
        (void)audio_start;
    }
    return true;
}

}  // namespace mtp
