#ifndef ARGUS_CORE_COMPOSE_CLIP_PROMPT_MOBILECLIP2_S2_ENGINE_H
#define ARGUS_CORE_COMPOSE_CLIP_PROMPT_MOBILECLIP2_S2_ENGINE_H

#include "core/decoder/video_frame.h"

#include <cstddef>
#include <memory>
#include <string>

namespace irs3 {

struct MobileClipPromptScore {
    double score = 0.0;
    double latency_ms = 0.0;
};

class MobileClip2S2Engine {
public:
    MobileClip2S2Engine();
    ~MobileClip2S2Engine();

    MobileClip2S2Engine(const MobileClip2S2Engine &) = delete;
    MobileClip2S2Engine &operator=(const MobileClip2S2Engine &) = delete;

    bool Prepare(const std::string &model_root, const std::string &prompt, std::string *error);
    void Close();
    bool ScoreFrame(const VideoFrame &frame, MobileClipPromptScore *score, std::string *error) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace irs3

#endif
