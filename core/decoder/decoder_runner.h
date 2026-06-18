#ifndef ARGUS_CORE_DECODER_DECODER_RUNNER_H
#define ARGUS_CORE_DECODER_DECODER_RUNNER_H

#include "core/decoder/decoded_video_consumer.h"
#include "core/decoder/h264_decoder.h"
#include "core/sources/isource.h"

#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace irs3 {

class DecoderRunner {
public:
    DecoderRunner(std::shared_ptr<ISource> source, SourceSubscriptionPtr subscription);
    ~DecoderRunner();

    DecoderRunner(const DecoderRunner &) = delete;
    DecoderRunner &operator=(const DecoderRunner &) = delete;

    void AddConsumer(const std::shared_ptr<IDecodedVideoConsumer> &consumer);
    void RemoveConsumer(const IDecodedVideoConsumer *consumer);
    bool Start();
    void Close();

private:
    void Run();
    void DispatchFrame(const VideoFrame &frame);

    std::shared_ptr<ISource> source_;
    SourceSubscriptionPtr subscription_;
    std::unique_ptr<H264Decoder> decoder_;
    std::mutex consumers_mutex_;
    std::vector<std::shared_ptr<IDecodedVideoConsumer>> consumers_;
    std::thread thread_;
    bool closed_ = false;
};

} // namespace irs3

#endif
