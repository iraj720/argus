#ifndef ARGUS_CORE_SINKS_HLS_SINK_RUNNER_H
#define ARGUS_CORE_SINKS_HLS_SINK_RUNNER_H

#include "core/sources/isource.h"

#include <memory>
#include <string>
#include <thread>

extern "C" {
#include "core/sinks/hls_sink.h"
}

namespace irs3 {

class HlsSinkRunner {
public:
    HlsSinkRunner(
        std::shared_ptr<ISource> source,
        SourceSubscriptionPtr subscription,
        std::string output_root,
        irs3_hls_sink_output_mode output_mode
    );
    ~HlsSinkRunner();

    bool Start();
    void Close();

private:
    void Run();
    bool InitSink(const SourceFormat &format);

    std::shared_ptr<ISource> source_;
    SourceSubscriptionPtr subscription_;
    std::string output_root_;
    irs3_hls_sink_output_mode output_mode_ = IRS3_HLS_SINK_OUTPUT_RECORD;
    std::thread thread_;
    irs3_hls_sink sink_{};
    bool sink_started_ = false;
    bool closed_ = false;
};

} // namespace irs3

#endif
