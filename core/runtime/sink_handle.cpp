#include "core/runtime/sink_handle.h"

#include <utility>

namespace irs3 {

SinkHandle::SinkHandle(SourcePtr source, SourceSubscriptionPtr subscription, RuntimeSinkSpec spec)
    : spec_(std::move(spec)),
      runner_(std::make_unique<HlsSinkRunner>(
          std::move(source),
          std::move(subscription),
          spec_.output_root,
          spec_.output_mode
      )) {
}

SinkHandle::~SinkHandle() {
    Close();
}

bool SinkHandle::Start() {
    if (runner_ == nullptr) {
        return false;
    }
    return runner_->Start();
}

void SinkHandle::Close() {
    if (closed_) {
        return;
    }
    closed_ = true;
    if (runner_ != nullptr) {
        runner_->Close();
    }
}

} // namespace irs3
