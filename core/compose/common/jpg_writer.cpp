#include "core/compose/common/jpg_writer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace irs3 {

namespace {

bool mkdir_p(const char *path) {
    char tmp[1024];
    const size_t len = std::strlen(path);
    if (len == 0 || len >= sizeof(tmp)) {
        return false;
    }
    std::snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return false;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

} // namespace

bool EnsureOutputDirectory(const std::string &path) {
    if (path.empty()) {
        return false;
    }
    return mkdir_p(path.c_str());
}

bool WriteJpegFrame(const VideoFrame &frame, const std::string &path, int quality) {
    if (frame.native == nullptr || path.empty()) {
        return false;
    }

    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (codec == nullptr) {
        return false;
    }

    AVCodecContext *encoder_ctx = avcodec_alloc_context3(codec);
    if (encoder_ctx == nullptr) {
        return false;
    }

    encoder_ctx->width = frame.native->width;
    encoder_ctx->height = frame.native->height;
    encoder_ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
    encoder_ctx->time_base = AVRational{1, 90000};
    av_opt_set_int(encoder_ctx, "qscale", quality, 0);

    if (avcodec_open2(encoder_ctx, codec, nullptr) < 0) {
        avcodec_free_context(&encoder_ctx);
        return false;
    }

    SwsContext *sws_ctx = sws_getContext(
        frame.native->width,
        frame.native->height,
        static_cast<AVPixelFormat>(frame.native->format),
        frame.native->width,
        frame.native->height,
        AV_PIX_FMT_YUVJ420P,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr
    );
    if (sws_ctx == nullptr) {
        avcodec_free_context(&encoder_ctx);
        return false;
    }

    AVFrame *converted = av_frame_alloc();
    if (converted == nullptr) {
        sws_freeContext(sws_ctx);
        avcodec_free_context(&encoder_ctx);
        return false;
    }

    converted->format = AV_PIX_FMT_YUVJ420P;
    converted->width = frame.native->width;
    converted->height = frame.native->height;
    if (av_frame_get_buffer(converted, 32) < 0) {
        av_frame_free(&converted);
        sws_freeContext(sws_ctx);
        avcodec_free_context(&encoder_ctx);
        return false;
    }

    sws_scale(
        sws_ctx,
        frame.native->data,
        frame.native->linesize,
        0,
        frame.native->height,
        converted->data,
        converted->linesize
    );

    AVPacket *packet = av_packet_alloc();
    if (packet == nullptr) {
        av_frame_free(&converted);
        sws_freeContext(sws_ctx);
        avcodec_free_context(&encoder_ctx);
        return false;
    }

    bool ok = false;
    if (avcodec_send_frame(encoder_ctx, converted) == 0 &&
        avcodec_receive_packet(encoder_ctx, packet) == 0) {
        FILE *fp = std::fopen(path.c_str(), "wb");
        if (fp != nullptr) {
            ok = std::fwrite(packet->data, 1, static_cast<size_t>(packet->size), fp) ==
                 static_cast<size_t>(packet->size);
            std::fclose(fp);
        }
    }

    av_packet_free(&packet);
    av_frame_free(&converted);
    sws_freeContext(sws_ctx);
    avcodec_free_context(&encoder_ctx);
    return ok;
}

} // namespace irs3
