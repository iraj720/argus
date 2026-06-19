#include "core/sinks/remux_sink_runner.h"

#include "core/sources/subscription_packet_reader.h"

#include <cstdio>
#include <utility>

namespace irs3 {

namespace {

RemuxPayloadMode remux_payload_mode(SourcePayloadMode mode) {
    switch (mode) {
    case SourcePayloadMode::kFlvTag:
        return RemuxPayloadMode::kFlvTag;
    case SourcePayloadMode::kPacketized:
        return RemuxPayloadMode::kPacketized;
    }
    return RemuxPayloadMode::kFlvTag;
}

irs3_hls_sink_stream_kind sink_kind(SourceTrackKind kind) {
    switch (kind) {
    case SourceTrackKind::kVideo:
        return IRS3_HLS_SINK_STREAM_VIDEO;
    case SourceTrackKind::kAudio:
        return IRS3_HLS_SINK_STREAM_AUDIO;
    case SourceTrackKind::kData:
        return IRS3_HLS_SINK_STREAM_AUDIO;
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

std::int64_t ms_to_stream_ticks(std::int64_t timestamp_ms, int clock_rate) {
    if (clock_rate <= 0) {
        return timestamp_ms;
    }
    return timestamp_ms * static_cast<std::int64_t>(clock_rate) / 1000;
}

int assign_output_stream_indices(const RuntimeSinkSpec &spec, std::vector<RemuxSinkInput> *inputs) {
    std::size_t video_count = 0;
    for (const RuntimeSinkInputSpec &input : spec.inputs) {
        if (input.stream_type == "video") {
            video_count += 1;
        }
    }

    int next_video = 0;
    int next_voice = static_cast<int>(video_count);
    for (RemuxSinkInput &input : *inputs) {
        if (input.input.stream_type == "video") {
            input.output_stream_index = next_video++;
        } else if (input.input.stream_type == "voice") {
            input.output_stream_index = next_voice++;
        }
    }
    return next_video + (next_voice - static_cast<int>(video_count));
}

} // namespace

RemuxSinkRunner::RemuxSinkRunner(RuntimeSinkSpec spec, std::vector<RemuxSinkInput> inputs)
    : spec_(std::move(spec)),
      inputs_(std::move(inputs)) {
}

RemuxSinkRunner::~RemuxSinkRunner() {
    Close();
}

std::string RemuxSinkRunner::output_dir() const {
    return spec_.output_root + "/mux/" + spec_.sink_id;
}

bool RemuxSinkRunner::Start() {
    thread_ = std::thread([this]() {
        Run();
    });
    return true;
}

void RemuxSinkRunner::Close() {
    if (closed_) {
        return;
    }
    closed_ = true;
    muxer_.Close();
    for (RemuxSinkInput &input : inputs_) {
        if (input.subscription != nullptr) {
            input.subscription->Close();
        }
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool RemuxSinkRunner::InitSink(const SourceFormat &format) {
    irs3_hls_sink_options sink_options{};
    irs3_hls_sink_default_options(&sink_options);
    sink_options.output_mode = spec_.output_mode;
    if (spec_.output_mode == IRS3_HLS_SINK_OUTPUT_LIVE) {
        sink_options.playlist_size = 3;
        sink_options.retain_extra_segments = 3;
    }

    const std::string dir = output_dir();
    if (format.payload_mode == SourcePayloadMode::kFlvTag) {
        if (irs3_hls_sink_init_at_dir_with_options(&sink_, dir.c_str(), &sink_options) != 0) {
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
        stream_configs[i].extradata =
            format.tracks[i].extradata.empty() ? nullptr : format.tracks[i].extradata.data();
        stream_configs[i].extradata_len = format.tracks[i].extradata.size();
    }

    if (irs3_hls_sink_init_packet_mode_at_dir_with_options(
            &sink_,
            dir.c_str(),
            &sink_options,
            stream_configs.data(),
            stream_configs.size()
        ) != 0) {
        return false;
    }
    sink_started_ = true;
    return true;
}

void RemuxSinkRunner::Run() {
    if (inputs_.empty()) {
        return;
    }

    SourceFormat format;
    RemuxPayloadMode payload_mode = RemuxPayloadMode::kFlvTag;
    for (RemuxSinkInput &input : inputs_) {
        SourceFormat source_format;
        if (!input.source->WaitReady(&source_format)) {
            return;
        }
        if (format.tracks.empty()) {
            format = source_format;
            payload_mode = remux_payload_mode(source_format.payload_mode);
        } else if (source_format.payload_mode != format.payload_mode) {
            std::fprintf(
                stderr,
                "argus: remux sink id=%s rejected mixed payload modes across inputs\n",
                spec_.sink_id.c_str()
            );
            return;
        }
    }

    (void)assign_output_stream_indices(spec_, &inputs_);

    if (!InitSink(format)) {
        std::fprintf(
            stderr,
            "argus: failed to initialize remux sink id=%s output=%s\n",
            spec_.sink_id.c_str(),
            output_dir().c_str()
        );
        return;
    }

    std::vector<RemuxMuxLane> lanes;
    lanes.reserve(inputs_.size());
    for (RemuxSinkInput &input : inputs_) {
        RemuxMuxLane lane;
        lane.input_id = input.input.id;
        lane.stream_type = input.input.stream_type;
        lane.output_stream_index = input.output_stream_index;
        lane.reader = MakeSubscriptionPacketReader(std::move(input.subscription));
        lanes.push_back(std::move(lane));
        input.subscription.reset();
    }

    if (!muxer_.Prepare(std::move(lanes), payload_mode)) {
        std::fprintf(stderr, "argus: failed to prepare remux muxer sink_id=%s\n", spec_.sink_id.c_str());
        return;
    }

    RemuxMuxPacket mux_packet;
    while (muxer_.NextMuxPacket(&mux_packet)) {
        const std::vector<std::uint8_t> empty_payload;
        const std::vector<std::uint8_t> &payload =
            mux_packet.packet.payload != nullptr ? *mux_packet.packet.payload : empty_payload;

        if (format.payload_mode == SourcePayloadMode::kFlvTag) {
            irs3_hls_sink_note_media(
                &sink_,
                static_cast<std::uint8_t>(mux_packet.packet.tag_type),
                mux_packet.packet.timestamp_ms,
                payload.size()
            );
            if (irs3_hls_sink_write_flv_tag(
                    &sink_,
                    static_cast<std::uint8_t>(mux_packet.packet.tag_type),
                    mux_packet.packet.timestamp_ms,
                    payload.data(),
                    payload.size()
                ) != 0) {
                break;
            }
        } else {
            const std::size_t stream_index = mux_packet.packet.stream_index;
            if (stream_index >= format.tracks.size()) {
                break;
            }
            const int clock_rate = format.tracks[stream_index].clock_rate;
            const std::int64_t pts = ms_to_stream_ticks(
                static_cast<std::int64_t>(mux_packet.packet.timestamp_ms),
                clock_rate
            );
            const std::int64_t duration = mux_packet.packet.duration > 0
                ? ms_to_stream_ticks(mux_packet.packet.duration, clock_rate)
                : 0;
            if (irs3_hls_sink_write_packet(
                    &sink_,
                    stream_index,
                    pts,
                    pts,
                    duration,
                    mux_packet.packet.key_frame ? 1 : 0,
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
