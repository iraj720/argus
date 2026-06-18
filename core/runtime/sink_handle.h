#ifndef ARGUS_CORE_RUNTIME_SINK_HANDLE_H
#define ARGUS_CORE_RUNTIME_SINK_HANDLE_H

#include "core/sources/isource.h"
#include "core/runtime/runtime_manifest.h"
#include "core/sinks/hls_sink_runner.h"

#include <memory>

namespace irs3 {

class SinkHandle {
public:
    SinkHandle(SourcePtr source, SourceSubscriptionPtr subscription, RuntimeSinkSpec spec);
    ~SinkHandle();

    SinkHandle(const SinkHandle &) = delete;
    SinkHandle &operator=(const SinkHandle &) = delete;

    bool Start();
    void Close();

private:
    RuntimeSinkSpec spec_;
    std::unique_ptr<HlsSinkRunner> runner_;
    bool closed_ = false;
};

} // namespace irs3

#endif
