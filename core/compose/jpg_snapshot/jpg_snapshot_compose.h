#ifndef ARGUS_CORE_COMPOSE_JPG_SNAPSHOT_JPG_SNAPSHOT_COMPOSE_H
#define ARGUS_CORE_COMPOSE_JPG_SNAPSHOT_JPG_SNAPSHOT_COMPOSE_H

#include "core/compose/icompose.h"

#include <cstddef>
#include <string>

namespace irs3 {

class JpgSnapshotCompose : public ICompose {
public:
    JpgSnapshotCompose(std::string output_root, std::size_t snapshot_interval = 50);

    bool Prepare() override;
    void Close() override;
    void OnVideoFrame(const VideoFrame &frame) override;

private:
    std::string output_root_;
    std::size_t snapshot_interval_ = 50;
    std::size_t decoded_frames_ = 0;
    bool prepared_ = false;
};

} // namespace irs3

#endif
