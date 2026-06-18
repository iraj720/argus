#ifndef ARGUS_CORE_COMPOSE_CLIP_PROMPT_CLIP_PROMPT_COMPOSE_H
#define ARGUS_CORE_COMPOSE_CLIP_PROMPT_CLIP_PROMPT_COMPOSE_H

#include "core/compose/icompose.h"

#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>

namespace irs3 {

class MobileClip2S2Engine;

class ClipPromptCompose : public ICompose {
public:
    ClipPromptCompose(
        std::string output_root,
        std::string prompt,
        std::string model_root,
        std::size_t snapshot_interval = 50
    );
    ~ClipPromptCompose() override;

    bool Prepare() override;
    void Close() override;
    void OnVideoFrame(const VideoFrame &frame) override;

private:
    std::string output_root_;
    std::string prompt_;
    std::string model_root_;
    std::size_t snapshot_interval_ = 50;
    std::size_t decoded_frames_ = 0;
    bool prepared_ = false;
    std::unique_ptr<MobileClip2S2Engine> engine_;
    std::FILE *log_file_ = nullptr;
};

} // namespace irs3

#endif
