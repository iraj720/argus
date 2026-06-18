#ifndef ARGUS_CORE_DECODER_DECODER_HANDLE_H
#define ARGUS_CORE_DECODER_DECODER_HANDLE_H

#include "core/decoder/decoder_runner.h"
#include "core/sources/isource.h"
#include "core/runtime/runtime_manifest.h"

#include <memory>

namespace irs3 {

class DecoderHandle {
public:
    DecoderHandle(SourcePtr source, SourceSubscriptionPtr subscription, RuntimeDecoderSpec spec);
    ~DecoderHandle();

    DecoderHandle(const DecoderHandle &) = delete;
    DecoderHandle &operator=(const DecoderHandle &) = delete;

    bool Start();
    void Close();
    void AddConsumer(const std::shared_ptr<IDecodedVideoConsumer> &consumer);
    void RemoveConsumer(const IDecodedVideoConsumer *consumer);

private:
    RuntimeDecoderSpec spec_;
    std::unique_ptr<DecoderRunner> runner_;
    bool closed_ = false;
};

} // namespace irs3

#endif
