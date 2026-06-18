#include "core/compose/side_by_side/side_by_side_frame.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <cstring>

namespace irs3 {

namespace {

bool copy_plane(
    uint8_t *dst,
    int dst_linesize,
    const uint8_t *src,
    int src_linesize,
    int plane_width,
    int plane_height
) {
    for (int row = 0; row < plane_height; ++row) {
        const uint8_t *src_row = src + row * src_linesize;
        uint8_t *left = dst + row * dst_linesize;
        uint8_t *right = left + plane_width;
        std::memcpy(left, src_row, static_cast<size_t>(plane_width));
        std::memcpy(right, src_row, static_cast<size_t>(plane_width));
    }
    return true;
}

} // namespace

AVFrame *BuildSideBySideFrame(const VideoFrame &frame) {
    if (frame.native == nullptr || frame.native->width <= 0 || frame.native->height <= 0) {
        return nullptr;
    }

    SwsContext *sws_ctx = sws_getContext(
        frame.native->width,
        frame.native->height,
        static_cast<AVPixelFormat>(frame.native->format),
        frame.native->width,
        frame.native->height,
        AV_PIX_FMT_YUV420P,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr
    );
    if (sws_ctx == nullptr) {
        return nullptr;
    }

    AVFrame *source_yuv = av_frame_alloc();
    if (source_yuv == nullptr) {
        sws_freeContext(sws_ctx);
        return nullptr;
    }

    source_yuv->format = AV_PIX_FMT_YUV420P;
    source_yuv->width = frame.native->width;
    source_yuv->height = frame.native->height;
    if (av_frame_get_buffer(source_yuv, 32) < 0) {
        av_frame_free(&source_yuv);
        sws_freeContext(sws_ctx);
        return nullptr;
    }

    sws_scale(
        sws_ctx,
        frame.native->data,
        frame.native->linesize,
        0,
        frame.native->height,
        source_yuv->data,
        source_yuv->linesize
    );
    sws_freeContext(sws_ctx);

    AVFrame *scene = av_frame_alloc();
    if (scene == nullptr) {
        av_frame_free(&source_yuv);
        return nullptr;
    }

    scene->format = AV_PIX_FMT_YUV420P;
    scene->width = source_yuv->width * 2;
    scene->height = source_yuv->height;
    scene->pts = frame.pts;
    if (av_frame_get_buffer(scene, 32) < 0) {
        av_frame_free(&scene);
        av_frame_free(&source_yuv);
        return nullptr;
    }

    copy_plane(
        scene->data[0],
        scene->linesize[0],
        source_yuv->data[0],
        source_yuv->linesize[0],
        source_yuv->width,
        source_yuv->height
    );
    copy_plane(
        scene->data[1],
        scene->linesize[1],
        source_yuv->data[1],
        source_yuv->linesize[1],
        source_yuv->width / 2,
        source_yuv->height / 2
    );
    copy_plane(
        scene->data[2],
        scene->linesize[2],
        source_yuv->data[2],
        source_yuv->linesize[2],
        source_yuv->width / 2,
        source_yuv->height / 2
    );

    av_frame_free(&source_yuv);
    return scene;
}

} // namespace irs3
