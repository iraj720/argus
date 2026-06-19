#ifndef ARGUS_CORE_SINKS_REMUX_SINK_RUNNER_H
#define ARGUS_CORE_SINKS_REMUX_SINK_RUNNER_H

#include "core/mux/remux_muxer.h"
#include "core/runtime/runtime_manifest.h"
#include "core/sources/isource.h"
#include "core/sinks/hls_sink.h"

#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace irs3 {

struct RemuxSinkInput {
    std::shared_ptr<ISource> source;
    SourceSubscriptionPtr subscription;
    RuntimeSinkInputSpec input;
    int output_stream_index = 0;
};

class RemuxSinkRunner {
public:
    RemuxSinkRunner(RuntimeSinkSpec spec, std::vector<RemuxSinkInput> inputs);
    ~RemuxSinkRunner();

    RemuxSinkRunner(const RemuxSinkRunner &) = delete;
    RemuxSinkRunner &operator=(const RemuxSinkRunner &) = delete;

    bool Start();
    void Close();

private:
    bool InitSink(const SourceFormat &format);
    void Run();
    std::string output_dir() const;

    RuntimeSinkSpec spec_;
    std::vector<RemuxSinkInput> inputs_;
    RemuxMuxer muxer_;
    irs3_hls_sink sink_{};
    bool sink_started_ = false;
    bool closed_ = false;
    std::thread thread_;
};

} // namespace irs3

#endif
