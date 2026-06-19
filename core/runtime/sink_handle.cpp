#include "core/runtime/sink_handle.h"

#include <utility>

namespace irs3 {

SinkHandle::SinkHandle(RuntimeSinkSpec spec, std::vector<RemuxSinkInput> inputs)
    : spec_(std::move(spec)),
      runner_(std::make_unique<RemuxSinkRunner>(spec_, std::move(inputs))) {
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
