#include "core/decoder/h264_decoder.h"

#include "core/decoder/packet_adapter.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/avutil.h>
}

#include <cstring>

namespace irs3 {

namespace {

bool payload_has_annexb_start_code(const std::vector<std::uint8_t> &payload) {
    if (payload.size() < 3) {
        return false;
    }
    if (payload[0] == 0 && payload[1] == 0 && payload[2] == 1) {
        return true;
    }
    return payload.size() >= 4 && payload[0] == 0 && payload[1] == 0 && payload[2] == 0 && payload[3] == 1;
}

} // namespace

H264Decoder::H264Decoder() = default;

H264Decoder::~H264Decoder() {
    ReleaseCodec();
}

bool H264Decoder::Configure(const SourceFormat &format) {
    format_ = format;
    configured_ = true;

    for (const SourceTrackConfig &track : format.tracks) {
        if (track.kind != SourceTrackKind::kVideo || track.codec != SourceCodec::kH264 || track.extradata.empty()) {
            continue;
        }
        if (track.extradata == extradata_) {
            return true;
        }
        return ApplyExtradata(track.extradata);
    }

    return true;
}

bool H264Decoder::FeedPacket(const SourcePacket &packet, SourcePayloadMode payload_mode) {
    if (!configured_) {
        return false;
    }

    H264AdaptedPacket adapted;
    if (!AdaptH264SourcePacket(packet, payload_mode, format_, &adapted)) {
        return true;
    }

    if (adapted.kind == H264PacketKind::kSequenceHeader) {
        if (adapted.extradata == extradata_) {
            return true;
        }
        return ApplyExtradata(adapted.extradata);
    }

    if (adapted.kind != H264PacketKind::kMedia) {
        return true;
    }

    if (extradata_.empty() && !adapted.extradata.empty()) {
        if (!ApplyExtradata(adapted.extradata)) {
            return false;
        }
    }

    if (codec_ctx_ == nullptr) {
        if (payload_has_annexb_start_code(adapted.payload)) {
            ReleaseCodec();
            const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
            if (codec == nullptr) {
                return false;
            }
            codec_ctx_ = avcodec_alloc_context3(codec);
            if (codec_ctx_ == nullptr || avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
                ReleaseCodec();
                return false;
            }
            packet_ = av_packet_alloc();
            frame_ = av_frame_alloc();
            use_annex_b_ = true;
        } else {
            return true;
        }
    }

    if (use_annex_b_) {
        return SendAnnexBPayload(adapted.payload, adapted.pts, adapted.dts);
    }

    if (extradata_.empty() || bsf_ctx_ == nullptr) {
        return true;
    }

    return SendPayload(adapted.payload, adapted.pts, adapted.dts);
}

bool H264Decoder::TryDecode(VideoFrame *out) {
    if (out == nullptr || codec_ctx_ == nullptr || frame_ == nullptr) {
        return false;
    }

    const int receive_result = avcodec_receive_frame(codec_ctx_, frame_);
    if (receive_result == 0) {
        out->width = frame_->width;
        out->height = frame_->height;
        out->pts = frame_->pts;
        out->native = frame_;
        return true;
    }
    if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
        return false;
    }
    return false;
}

void H264Decoder::Reset() {
    ReleaseCodec();
    configured_ = false;
    extradata_.clear();
    format_ = SourceFormat{};
}

bool H264Decoder::ApplyExtradata(const std::vector<std::uint8_t> &extradata) {
    if (extradata.empty()) {
        return true;
    }

    ReleaseCodec();
    extradata_ = extradata;
    use_annex_b_ = false;

    const AVBitStreamFilter *bsf = av_bsf_get_by_name("h264_mp4toannexb");
    if (bsf == nullptr) {
        return false;
    }

    if (av_bsf_alloc(bsf, &bsf_ctx_) < 0) {
        ReleaseCodec();
        return false;
    }

    AVCodecParameters *parameters = avcodec_parameters_alloc();
    if (parameters == nullptr) {
        ReleaseCodec();
        return false;
    }

    parameters->codec_type = AVMEDIA_TYPE_VIDEO;
    parameters->codec_id = AV_CODEC_ID_H264;
    parameters->extradata = static_cast<std::uint8_t *>(
        av_mallocz(extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE)
    );
    if (parameters->extradata == nullptr) {
        avcodec_parameters_free(&parameters);
        ReleaseCodec();
        return false;
    }
    std::memcpy(parameters->extradata, extradata.data(), extradata.size());
    parameters->extradata_size = static_cast<int>(extradata.size());

    if (avcodec_parameters_copy(bsf_ctx_->par_in, parameters) < 0) {
        avcodec_parameters_free(&parameters);
        ReleaseCodec();
        return false;
    }
    avcodec_parameters_free(&parameters);

    if (av_bsf_init(bsf_ctx_) < 0) {
        ReleaseCodec();
        return false;
    }

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (codec == nullptr) {
        ReleaseCodec();
        return false;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (codec_ctx_ == nullptr || avcodec_open2(codec_ctx_, codec, nullptr) < 0) {
        ReleaseCodec();
        return false;
    }

    packet_ = av_packet_alloc();
    filtered_packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (packet_ == nullptr || filtered_packet_ == nullptr || frame_ == nullptr) {
        ReleaseCodec();
        return false;
    }

    return true;
}

bool H264Decoder::SendPayload(const std::vector<std::uint8_t> &payload, std::int64_t pts, std::int64_t dts) {
    if (packet_ == nullptr || filtered_packet_ == nullptr || codec_ctx_ == nullptr || bsf_ctx_ == nullptr ||
        payload.empty()) {
        return false;
    }

    av_packet_unref(packet_);
    if (av_new_packet(packet_, static_cast<int>(payload.size())) < 0) {
        return false;
    }
    std::memcpy(packet_->data, payload.data(), payload.size());
    packet_->pts = pts;
    packet_->dts = dts;

    const int send_result = av_bsf_send_packet(bsf_ctx_, packet_);
    if (send_result < 0 && send_result != AVERROR(EAGAIN)) {
        return false;
    }

    while (true) {
        av_packet_unref(filtered_packet_);
        const int receive_result = av_bsf_receive_packet(bsf_ctx_, filtered_packet_);
        if (receive_result == AVERROR(EAGAIN) || receive_result == AVERROR_EOF) {
            return true;
        }
        if (receive_result < 0) {
            return false;
        }

        const int decode_result = avcodec_send_packet(codec_ctx_, filtered_packet_);
        if (decode_result < 0 && decode_result != AVERROR(EAGAIN)) {
            return false;
        }
    }
}

bool H264Decoder::SendAnnexBPayload(const std::vector<std::uint8_t> &payload, std::int64_t pts, std::int64_t dts) {
    if (packet_ == nullptr || codec_ctx_ == nullptr || payload.empty()) {
        return false;
    }

    av_packet_unref(packet_);
    if (av_new_packet(packet_, static_cast<int>(payload.size())) < 0) {
        return false;
    }
    std::memcpy(packet_->data, payload.data(), payload.size());
    packet_->pts = pts;
    packet_->dts = dts;

    const int send_result = avcodec_send_packet(codec_ctx_, packet_);
    return send_result == 0 || send_result == AVERROR(EAGAIN);
}

void H264Decoder::ReleaseCodec() {
    if (frame_ != nullptr) {
        av_frame_free(&frame_);
    }
    if (filtered_packet_ != nullptr) {
        av_packet_free(&filtered_packet_);
    }
    if (packet_ != nullptr) {
        av_packet_free(&packet_);
    }
    if (codec_ctx_ != nullptr) {
        avcodec_free_context(&codec_ctx_);
    }
    if (bsf_ctx_ != nullptr) {
        av_bsf_free(&bsf_ctx_);
    }
    use_annex_b_ = false;
}

} // namespace irs3
