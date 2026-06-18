#include "core/compose/clip_prompt/clip_prompt_compose.h"

#include "core/compose/clip_prompt/mobileclip2_s2_engine.h"
#include "core/compose/clip_prompt/model_path.h"
#include "core/compose/common/jpg_writer.h"

#include <cstdio>
#include <utility>

namespace irs3 {

namespace {

constexpr const char *kDefaultModelRoot = "./models/mobileclip2_s2";

} // namespace

ClipPromptCompose::ClipPromptCompose(
    std::string output_root,
    std::string prompt,
    std::string model_root,
    std::size_t snapshot_interval
)
    : output_root_(std::move(output_root)),
      prompt_(std::move(prompt)),
      model_root_(model_root.empty() ? kDefaultModelRoot : std::move(model_root)),
      snapshot_interval_(snapshot_interval > 0 ? snapshot_interval : 50),
      engine_(std::make_unique<MobileClip2S2Engine>()) {
}

ClipPromptCompose::~ClipPromptCompose() {
    Close();
}

bool ClipPromptCompose::Prepare() {
    if (prompt_.empty()) {
        std::fprintf(stderr, "argus: clip_prompt compose requires a non-empty prompt\n");
        return false;
    }
    if (!EnsureOutputDirectory(output_root_)) {
        std::fprintf(
            stderr,
            "argus: failed to create clip_prompt output dir output=%s\n",
            output_root_.c_str()
        );
        return false;
    }

    std::string error;
    model_root_ = ResolveMobileClip2ModelRoot(model_root_);
    if (!engine_->Prepare(model_root_, prompt_, &error)) {
        std::fprintf(
            stderr,
            "argus: failed to prepare MobileCLIP2-S2 engine model_root=%s error=%s\n",
            model_root_.c_str(),
            error.c_str()
        );
        return false;
    }

    char log_path[1024];
    std::snprintf(log_path, sizeof(log_path), "%s/infer.log", output_root_.c_str());
    log_file_ = std::fopen(log_path, "a");
    if (log_file_ == nullptr) {
        std::fprintf(stderr, "argus: failed to open clip_prompt log path=%s\n", log_path);
        engine_->Close();
        return false;
    }

    prepared_ = true;

    std::fprintf(
        stderr,
        "argus: clip_prompt ready prompt=\"%s\" model_root=%s\n",
        prompt_.c_str(),
        model_root_.c_str()
    );
    return true;
}

void ClipPromptCompose::Close() {
    if (log_file_ != nullptr) {
        std::fclose(log_file_);
        log_file_ = nullptr;
    }
    if (engine_ != nullptr) {
        engine_->Close();
    }
    prepared_ = false;
}

void ClipPromptCompose::OnVideoFrame(const VideoFrame &frame) {
    if (!prepared_ || engine_ == nullptr) {
        return;
    }

    ++decoded_frames_;
    if (decoded_frames_ % snapshot_interval_ != 0) {
        return;
    }

    MobileClipPromptScore score;
    std::string error;
    if (!engine_->ScoreFrame(frame, &score, &error)) {
        std::fprintf(
            stderr,
            "argus: clip_prompt inference failed frame=%zu error=%s\n",
            decoded_frames_,
            error.c_str()
        );
        return;
    }

    std::fprintf(
        stderr,
        "argus: clip_prompt frame=%zu prompt=\"%s\" score=%.4f latency_ms=%.1f\n",
        decoded_frames_,
        prompt_.c_str(),
        score.score,
        score.latency_ms
    );

    if (log_file_ != nullptr) {
        std::fprintf(
            log_file_,
            "frame=%zu prompt=\"%s\" score=%.6f latency_ms=%.3f\n",
            decoded_frames_,
            prompt_.c_str(),
            score.score,
            score.latency_ms
        );
        std::fflush(log_file_);
    }
}

} // namespace irs3
