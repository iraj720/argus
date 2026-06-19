#include "core/mux/remux_muxer.h"

#include <algorithm>
#include <limits>

namespace irs3 {

bool RemuxMuxer::Prepare(std::vector<RemuxMuxLane> lanes, RemuxPayloadMode payload_mode) {
    Close();
    if (lanes.empty()) {
        return false;
    }

    payload_mode_ = payload_mode;
    lanes_.clear();
    lanes_.reserve(lanes.size());

    std::size_t main_lane_index = lanes.size();
    for (std::size_t i = 0; i < lanes.size(); ++i) {
        if (lanes[i].reader == nullptr) {
            return false;
        }
        LaneState state;
        state.lane = std::move(lanes[i]);
        if (state.lane.stream_type == "video" && main_lane_index == lanes.size()) {
            main_lane_index = i;
        }
        lanes_.push_back(std::move(state));
    }

    if (main_lane_index == lanes.size()) {
        main_lane_index = 0;
    }
    lanes_[main_lane_index].is_main = true;
    return true;
}

void RemuxMuxer::Close() {
    closed_ = true;
    for (LaneState &lane : lanes_) {
        if (lane.lane.reader != nullptr) {
            lane.lane.reader->Close();
            lane.lane.reader.reset();
        }
    }
    lanes_.clear();
    main_lane_ready_ = false;
    main_first_timestamp_ms_ = 0;
    closed_ = false;
}

std::int64_t RemuxMuxer::packet_timestamp_ms(const SourcePacket &packet) {
    return static_cast<std::int64_t>(packet.timestamp_ms);
}

void RemuxMuxer::apply_remapped_timestamp_ms(
    const LaneState &lane,
    std::int64_t remapped_timestamp_ms,
    SourcePacket *packet
) {
    if (packet == nullptr) {
        return;
    }
    packet->timestamp_ms = static_cast<std::uint32_t>(remapped_timestamp_ms);
    packet->pts = remapped_timestamp_ms;
    packet->dts = remapped_timestamp_ms;
    packet->stream_index = static_cast<std::size_t>(lane.lane.output_stream_index);
}

bool RemuxMuxer::all_lanes_have_first_timestamp() const {
    for (const LaneState &lane : lanes_) {
        if (!lane.has_first_timestamp) {
            return false;
        }
    }
    return true;
}

void RemuxMuxer::update_lane_offset(LaneState *lane) {
    if (lane == nullptr || lane->is_main) {
        return;
    }
    lane->timestamp_offset_ms = main_first_timestamp_ms_ - lane->first_timestamp_ms;
}

bool RemuxMuxer::ensure_pending(LaneState *lane) {
    if (lane == nullptr || lane->has_pending || lane->lane.reader == nullptr) {
        return lane != nullptr && lane->has_pending;
    }

    SourcePacket packet;
    if (!lane->lane.reader->Read(&packet)) {
        return false;
    }

    if (!lane->has_first_timestamp) {
        lane->first_timestamp_ms = packet_timestamp_ms(packet);
        lane->has_first_timestamp = true;
        if (lane->is_main) {
            main_first_timestamp_ms_ = lane->first_timestamp_ms;
            main_lane_ready_ = true;
            for (LaneState &other : lanes_) {
                if (!other.is_main && other.has_first_timestamp) {
                    update_lane_offset(&other);
                }
            }
        } else if (main_lane_ready_) {
            update_lane_offset(lane);
        }
    }

    lane->pending = std::move(packet);
    lane->has_pending = true;
    return true;
}

bool RemuxMuxer::NextMuxPacket(RemuxMuxPacket *out) {
    if (out == nullptr || lanes_.empty()) {
        return false;
    }

    for (;;) {
        bool any_readable = false;
        for (LaneState &lane : lanes_) {
            if (ensure_pending(&lane)) {
                any_readable = true;
            }
        }
        if (!any_readable) {
            return false;
        }

        if (!main_lane_ready_) {
            continue;
        }

        if (!all_lanes_have_first_timestamp()) {
            for (LaneState &lane : lanes_) {
                if (!lane.has_first_timestamp) {
                    (void)ensure_pending(&lane);
                }
            }
            if (!all_lanes_have_first_timestamp()) {
                continue;
            }
        }

        std::size_t selected_index = lanes_.size();
        std::int64_t selected_timestamp_ms = std::numeric_limits<std::int64_t>::max();
        for (std::size_t i = 0; i < lanes_.size(); ++i) {
            LaneState &lane = lanes_[i];
            if (!lane.has_pending) {
                continue;
            }
            const std::int64_t remapped =
                packet_timestamp_ms(lane.pending) + (lane.is_main ? 0 : lane.timestamp_offset_ms);
            if (remapped < selected_timestamp_ms) {
                selected_timestamp_ms = remapped;
                selected_index = i;
            }
        }

        if (selected_index >= lanes_.size()) {
            continue;
        }

        LaneState &selected = lanes_[selected_index];
        out->lane_index = selected_index;
        out->packet = std::move(selected.pending);
        out->remapped_timestamp_ms = selected_timestamp_ms;
        apply_remapped_timestamp_ms(selected, selected_timestamp_ms, &out->packet);
        selected.has_pending = false;
        return true;
    }
}

} // namespace irs3
