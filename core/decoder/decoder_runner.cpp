#include "core/decoder/decoder_runner.h"

#include "core/decoder/video_frame_utils.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace irs3 {

DecoderRunner::DecoderRunner(std::shared_ptr<ISource> source, SourceSubscriptionPtr subscription)
    : source_(std::move(source)),
      subscription_(std::move(subscription)),
      decoder_(std::make_unique<H264Decoder>()) {
}

DecoderRunner::~DecoderRunner() {
    Close();
}

void DecoderRunner::AddConsumer(const std::shared_ptr<IDecodedVideoConsumer> &consumer) {
    if (consumer == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(consumers_mutex_);
    consumers_.push_back(consumer);
}

void DecoderRunner::RemoveConsumer(const IDecodedVideoConsumer *consumer) {
    if (consumer == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(consumers_mutex_);
    consumers_.erase(
        std::remove_if(
            consumers_.begin(),
            consumers_.end(),
            [consumer](const std::shared_ptr<IDecodedVideoConsumer> &entry) {
                return entry.get() == consumer;
            }
        ),
        consumers_.end()
    );
}

bool DecoderRunner::Start() {
    thread_ = std::thread([this]() {
        Run();
    });
    return true;
}

void DecoderRunner::Close() {
    if (closed_) {
        return;
    }
    closed_ = true;
    if (subscription_ != nullptr) {
        subscription_->Close();
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

void DecoderRunner::DispatchFrame(const VideoFrame &frame) {
    std::vector<std::shared_ptr<IDecodedVideoConsumer>> consumers;
    {
        std::lock_guard<std::mutex> lock(consumers_mutex_);
        consumers = consumers_;
    }

    if (consumers.empty()) {
        return;
    }

    if (consumers.size() == 1) {
        if (consumers.front() != nullptr) {
            consumers.front()->OnVideoFrame(frame);
        }
        return;
    }

    std::vector<std::thread> workers;
    workers.reserve(consumers.size());
    for (const std::shared_ptr<IDecodedVideoConsumer> &consumer : consumers) {
        if (consumer == nullptr) {
            continue;
        }
        VideoFrame cloned = CloneVideoFrame(frame);
        workers.emplace_back([consumer, cloned]() mutable {
            consumer->OnVideoFrame(cloned);
            ReleaseVideoFrame(&cloned);
        });
    }
    for (std::thread &worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void DecoderRunner::Run() {
    SourceFormat format;
    if (!source_->WaitReady(&format)) {
        return;
    }
    if (!decoder_->Configure(format)) {
        std::fprintf(
            stderr,
            "argus: failed to configure decoder source=%s\n",
            source_->Descriptor().id.c_str()
        );
        return;
    }

    SourcePacket packet;
    while (subscription_ != nullptr && subscription_->Read(&packet)) {
        if (source_->WaitReady(&format)) {
            (void)decoder_->Configure(format);
        }

        if (!decoder_->FeedPacket(packet, format.payload_mode)) {
            continue;
        }

        VideoFrame frame;
        while (decoder_->TryDecode(&frame)) {
            DispatchFrame(frame);
        }
    }
}

} // namespace irs3
