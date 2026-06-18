#include "core/compose/side_by_side/side_by_side_compose.h"

#include "core/compose/common/jpg_writer.h"
#include "core/compose/side_by_side/side_by_side_frame.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <cstdio>

namespace irs3 {

SideBySideCompose::SideBySideCompose(std::string output_root, std::size_t snapshot_interval)
    : output_root_(std::move(output_root)),
      snapshot_interval_(snapshot_interval > 0 ? snapshot_interval : 50) {
}

bool SideBySideCompose::Prepare() {
    if (!EnsureOutputDirectory(output_root_)) {
        std::fprintf(
            stderr,
            "argus: failed to create side-by-side compose output dir output=%s\n",
            output_root_.c_str()
        );
        return false;
    }
    prepared_ = true;
    return true;
}

void SideBySideCompose::Close() {
    prepared_ = false;
}

void SideBySideCompose::OnVideoFrame(const VideoFrame &frame) {
    if (!prepared_) {
        return;
    }

    ++decoded_frames_;
    if (decoded_frames_ % snapshot_interval_ != 0) {
        return;
    }

    AVFrame *scene = BuildSideBySideFrame(frame);
    if (scene == nullptr) {
        std::fprintf(stderr, "argus: failed to build side-by-side scene frame\n");
        return;
    }

    VideoFrame composed;
    composed.width = scene->width;
    composed.height = scene->height;
    composed.pts = frame.pts;
    composed.native = scene;

    char path[1024];
    std::snprintf(
        path,
        sizeof(path),
        "%s/scene_%06zu.jpg",
        output_root_.c_str(),
        decoded_frames_
    );
    if (!WriteJpegFrame(composed, path)) {
        std::fprintf(stderr, "argus: failed to write side-by-side scene path=%s\n", path);
    }

    av_frame_free(&scene);
}

} // namespace irs3
