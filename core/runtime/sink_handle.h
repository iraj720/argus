#ifndef ARGUS_CORE_RUNTIME_SINK_HANDLE_H
#define ARGUS_CORE_RUNTIME_SINK_HANDLE_H

#include "core/runtime/runtime_manifest.h"
#include "core/sinks/remux_sink_runner.h"

#include <memory>
#include <vector>

namespace irs3 {

class SinkHandle {
public:
    SinkHandle(RuntimeSinkSpec spec, std::vector<RemuxSinkInput> inputs);
    ~SinkHandle();

    SinkHandle(const SinkHandle &) = delete;
    SinkHandle &operator=(const SinkHandle &) = delete;

    bool Start();
    void Close();

private:
    RuntimeSinkSpec spec_;
    std::unique_ptr<RemuxSinkRunner> runner_;
    bool closed_ = false;
};

} // namespace irs3

#endif
