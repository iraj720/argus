#include "core/compose/side_by_side/side_by_side_frame.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
}

#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

irs3::VideoFrame make_test_frame(int width, int height) {
    AVFrame *native = av_frame_alloc();
    native->format = AV_PIX_FMT_YUV420P;
    native->width = width;
    native->height = height;
    av_frame_get_buffer(native, 32);

    for (int row = 0; row < height; ++row) {
        std::memset(native->data[0] + row * native->linesize[0], 128, static_cast<size_t>(width));
    }
    for (int row = 0; row < height / 2; ++row) {
        std::memset(native->data[1] + row * native->linesize[1], 64, static_cast<size_t>(width / 2));
        std::memset(native->data[2] + row * native->linesize[2], 192, static_cast<size_t>(width / 2));
    }

    irs3::VideoFrame frame;
    frame.width = width;
    frame.height = height;
    frame.pts = 42;
    frame.native = native;
    return frame;
}

} // namespace

int main() {
    irs3::VideoFrame source = make_test_frame(160, 90);
    AVFrame *scene = irs3::BuildSideBySideFrame(source);
    expect(scene != nullptr, "side-by-side frame should be built");
    if (scene != nullptr) {
        expect(scene->width == source.width * 2, "scene width should be doubled");
        expect(scene->height == source.height, "scene height should match source");
        expect(scene->format == AV_PIX_FMT_YUV420P, "scene should use YUV420P");
        av_frame_free(&scene);
    }

    av_frame_free(&source.native);

    if (failures != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }

    std::fprintf(stderr, "side_by_side_frame_test passed\n");
    return 0;
}
