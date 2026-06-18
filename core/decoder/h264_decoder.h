#ifndef ARGUS_CORE_DECODER_H264_DECODER_H
#define ARGUS_CORE_DECODER_H264_DECODER_H

#include "core/decoder/idecoder.h"

#include <cstdint>
#include <vector>

struct AVBSFContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace irs3 {

class H264Decoder : public IDecoder {
public:
    H264Decoder();
    ~H264Decoder() override;

    H264Decoder(const H264Decoder &) = delete;
    H264Decoder &operator=(const H264Decoder &) = delete;

    bool Configure(const SourceFormat &format) override;
    bool FeedPacket(const SourcePacket &packet, SourcePayloadMode payload_mode) override;
    bool TryDecode(VideoFrame *out) override;
    void Reset() override;

private:
    bool ApplyExtradata(const std::vector<std::uint8_t> &extradata);
    bool SendPayload(const std::vector<std::uint8_t> &payload, std::int64_t pts, std::int64_t dts);
    bool SendAnnexBPayload(const std::vector<std::uint8_t> &payload, std::int64_t pts, std::int64_t dts);
    void ReleaseCodec();

    SourceFormat format_{};
    bool configured_ = false;
    bool use_annex_b_ = false;
    std::vector<std::uint8_t> extradata_;
    AVCodecContext *codec_ctx_ = nullptr;
    AVBSFContext *bsf_ctx_ = nullptr;
    AVPacket *packet_ = nullptr;
    AVPacket *filtered_packet_ = nullptr;
    AVFrame *frame_ = nullptr;
};

} // namespace irs3

#endif
