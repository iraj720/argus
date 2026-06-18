#include "core/compose/jpg_snapshot/jpg_snapshot_compose.h"

#include "core/compose/common/jpg_writer.h"

#include <cstdio>

namespace irs3 {

JpgSnapshotCompose::JpgSnapshotCompose(std::string output_root, std::size_t snapshot_interval)
    : output_root_(std::move(output_root)),
      snapshot_interval_(snapshot_interval > 0 ? snapshot_interval : 50) {
}

bool JpgSnapshotCompose::Prepare() {
    if (!EnsureOutputDirectory(output_root_)) {
        std::fprintf(
            stderr,
            "argus: failed to create compose output dir output=%s\n",
            output_root_.c_str()
        );
        return false;
    }
    prepared_ = true;
    return true;
}

void JpgSnapshotCompose::Close() {
    prepared_ = false;
}

void JpgSnapshotCompose::OnVideoFrame(const VideoFrame &frame) {
    if (!prepared_) {
        return;
    }

    ++decoded_frames_;
    if (decoded_frames_ % snapshot_interval_ != 0) {
        return;
    }

    char path[1024];
    std::snprintf(
        path,
        sizeof(path),
        "%s/frame_%06zu.jpg",
        output_root_.c_str(),
        decoded_frames_
    );
    if (!WriteJpegFrame(frame, path)) {
        std::fprintf(stderr, "argus: failed to write compose snapshot path=%s\n", path);
    }
}

} // namespace irs3
