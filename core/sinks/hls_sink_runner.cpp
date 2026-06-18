#include "core/sinks/hls_sink_runner.h"

#include <cstdio>
#include <vector>

namespace irs3 {

namespace {

irs3_hls_sink_stream_kind sink_kind(SourceTrackKind kind) {
    switch (kind) {
    case SourceTrackKind::kVideo:
        return IRS3_HLS_SINK_STREAM_VIDEO;
    case SourceTrackKind::kAudio:
        return IRS3_HLS_SINK_STREAM_AUDIO;
    case SourceTrackKind::kData:
        return IRS3_HLS_SINK_STREAM_VIDEO;
    }
    return IRS3_HLS_SINK_STREAM_VIDEO;
}

irs3_hls_sink_codec sink_codec(SourceCodec codec) {
    switch (codec) {
    case SourceCodec::kH264:
        return IRS3_HLS_SINK_CODEC_H264;
    case SourceCodec::kAAC:
        return IRS3_HLS_SINK_CODEC_AAC;
    case SourceCodec::kOpus:
        return IRS3_HLS_SINK_CODEC_OPUS;
    case SourceCodec::kUnknown:
        return IRS3_HLS_SINK_CODEC_H264;
    }
    return IRS3_HLS_SINK_CODEC_H264;
}

} // namespace

HlsSinkRunner::HlsSinkRunner(
    std::shared_ptr<ISource> source,
    SourceSubscriptionPtr subscription,
    std::string output_root,
    irs3_hls_sink_output_mode output_mode
)
    : source_(std::move(source)),
      subscription_(std::move(subscription)),
      output_root_(std::move(output_root)),
      output_mode_(output_mode) {
}

HlsSinkRunner::~HlsSinkRunner() {
    Close();
}

bool HlsSinkRunner::Start() {
    thread_ = std::thread([this]() {
        Run();
    });
    return true;
}

void HlsSinkRunner::Close() {
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

bool HlsSinkRunner::InitSink(const SourceFormat &format) {
    const SourceDescriptor &descriptor = source_->Descriptor();
    irs3_hls_sink_options sink_options{};
    irs3_hls_sink_default_options(&sink_options);
    sink_options.output_mode = output_mode_;
    if (output_mode_ == IRS3_HLS_SINK_OUTPUT_LIVE) {
        sink_options.playlist_size = 3;
        sink_options.retain_extra_segments = 3;
    }

    if (format.payload_mode == SourcePayloadMode::kFlvTag) {
        if (irs3_hls_sink_init_with_options(
                &sink_,
                output_root_.c_str(),
                descriptor.app.c_str(),
                descriptor.stream.c_str(),
                descriptor.session_id,
                &sink_options
            ) != 0) {
            return false;
        }
        sink_started_ = true;
        return true;
    }

    std::vector<irs3_hls_sink_stream_config> stream_configs(format.tracks.size());
    for (std::size_t i = 0; i < format.tracks.size(); ++i) {
        stream_configs[i].kind = sink_kind(format.tracks[i].kind);
        stream_configs[i].codec = sink_codec(format.tracks[i].codec);
        stream_configs[i].clock_rate = format.tracks[i].clock_rate;
        stream_configs[i].channels = format.tracks[i].channels;
        stream_configs[i].width = format.tracks[i].width;
        stream_configs[i].height = format.tracks[i].height;
        stream_configs[i].extradata = format.tracks[i].extradata.empty() ? nullptr : format.tracks[i].extradata.data();
        stream_configs[i].extradata_len = format.tracks[i].extradata.size();
    }

    if (irs3_hls_sink_init_packet_mode_with_options(
            &sink_,
            output_root_.c_str(),
            descriptor.app.c_str(),
            descriptor.stream.c_str(),
            descriptor.session_id,
            &sink_options,
            stream_configs.data(),
            stream_configs.size()
        ) != 0) {
        return false;
    }
    sink_started_ = true;
    return true;
}

void HlsSinkRunner::Run() {
    SourceFormat format;
    if (!source_->WaitReady(&format)) {
        return;
    }
    if (!InitSink(format)) {
        std::fprintf(stderr, "argus: failed to initialize hls sink source=%s output=%s\n",
            source_->Descriptor().id.c_str(), output_root_.c_str());
        return;
    }

    SourcePacket packet;
    while (subscription_ != nullptr && subscription_->Read(&packet)) {
        const std::vector<std::uint8_t> empty_payload;
        const std::vector<std::uint8_t> &payload = packet.payload != nullptr ? *packet.payload : empty_payload;
        if (format.payload_mode == SourcePayloadMode::kFlvTag) {
            irs3_hls_sink_note_media(&sink_, static_cast<std::uint8_t>(packet.tag_type), packet.timestamp_ms, payload.size());
            if (irs3_hls_sink_write_flv_tag(&sink_, static_cast<std::uint8_t>(packet.tag_type), packet.timestamp_ms, payload.data(), payload.size()) != 0) {
                break;
            }
        } else {
            if (irs3_hls_sink_write_packet(
                    &sink_,
                    packet.stream_index,
                    packet.pts,
                    packet.dts,
                    packet.duration,
                    packet.key_frame ? 1 : 0,
                    payload.data(),
                    payload.size()
                ) != 0) {
                break;
            }
        }
    }

    if (sink_started_) {
        irs3_hls_sink_close(&sink_);
        sink_started_ = false;
    }
}

} // namespace irs3
