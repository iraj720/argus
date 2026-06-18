#ifndef ARGUS_CORE_COMPOSE_SIDE_BY_SIDE_SIDE_BY_SIDE_COMPOSE_H
#define ARGUS_CORE_COMPOSE_SIDE_BY_SIDE_SIDE_BY_SIDE_COMPOSE_H

#include "core/compose/icompose.h"

#include <cstddef>
#include <string>

namespace irs3 {

class SideBySideCompose : public ICompose {
public:
    SideBySideCompose(std::string output_root, std::size_t snapshot_interval = 50);

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
