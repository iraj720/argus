#include "core/compose/compose_handle.h"

#include "core/compose/compose_type.h"
#include "core/compose/jpg_snapshot/jpg_snapshot_compose.h"
#include "core/compose/side_by_side/side_by_side_compose.h"
#include "core/compose/clip_prompt/clip_prompt_compose.h"

#include <utility>

namespace irs3 {

namespace {

std::shared_ptr<ICompose> MakeCompose(const RuntimeComposeSpec &spec) {
    if (spec.compose_type == kComposeTypeSideBySide) {
        return std::make_shared<SideBySideCompose>(spec.output_root, spec.snapshot_interval);
    }
    if (spec.compose_type == kComposeTypeClipPrompt) {
        return std::make_shared<ClipPromptCompose>(
            spec.output_root,
            spec.prompt,
            spec.model_root,
            spec.snapshot_interval
        );
    }
    return std::make_shared<JpgSnapshotCompose>(spec.output_root, spec.snapshot_interval);
}

} // namespace

ComposeHandle::ComposeHandle(DecoderHandle *decoder_handle, RuntimeComposeSpec spec)
    : spec_(std::move(spec)),
      decoder_handle_(decoder_handle),
      compose_(MakeCompose(spec_)) {
}

ComposeHandle::~ComposeHandle() {
    Close();
}

bool ComposeHandle::Start() {
    if (decoder_handle_ == nullptr || compose_ == nullptr) {
        return false;
    }
    if (!compose_->Prepare()) {
        return false;
    }
    decoder_handle_->AddConsumer(compose_);
    return true;
}

void ComposeHandle::Close() {
    if (closed_) {
        return;
    }
    closed_ = true;
    if (decoder_handle_ != nullptr && compose_ != nullptr) {
        decoder_handle_->RemoveConsumer(compose_.get());
    }
    if (compose_ != nullptr) {
        compose_->Close();
    }
}

} // namespace irs3
