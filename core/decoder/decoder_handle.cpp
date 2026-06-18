#include "core/decoder/decoder_handle.h"

#include <utility>

namespace irs3 {

DecoderHandle::DecoderHandle(SourcePtr source, SourceSubscriptionPtr subscription, RuntimeDecoderSpec spec)
    : spec_(std::move(spec)),
      runner_(std::make_unique<DecoderRunner>(std::move(source), std::move(subscription))) {
}

DecoderHandle::~DecoderHandle() {
    Close();
}

bool DecoderHandle::Start() {
    if (runner_ == nullptr) {
        return false;
    }
    return runner_->Start();
}

void DecoderHandle::Close() {
    if (closed_) {
        return;
    }
    closed_ = true;
    if (runner_ != nullptr) {
        runner_->Close();
    }
}

void DecoderHandle::AddConsumer(const std::shared_ptr<IDecodedVideoConsumer> &consumer) {
    if (runner_ != nullptr) {
        runner_->AddConsumer(consumer);
    }
}

void DecoderHandle::RemoveConsumer(const IDecodedVideoConsumer *consumer) {
    if (runner_ != nullptr) {
        runner_->RemoveConsumer(consumer);
    }
}

} // namespace irs3
