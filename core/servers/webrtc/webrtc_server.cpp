#include "core/servers/webrtc/webrtc_server.h"

#include "core/sources/buffered_source.h"
#include "core/sources/source_queue.h"

extern "C" {
#include <libavutil/base64.h>
}

#include <rtc/h264rtpdepacketizer.hpp>
#include <rtc/peerconnection.hpp>
#include <rtc/rtc.hpp>
#include <rtc/rtcpreceivingsession.hpp>
#include <rtc/rtpdepacketizer.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr size_t kMaxRequestBytes = 1024 * 1024;
constexpr size_t kMaxResponseBytes = 1024 * 1024;
struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;
    std::string body;
};

struct HttpResponse {
    int status = 200;
    std::string reason = "OK";
    std::map<std::string, std::string> headers;
    std::string body;
};

struct PathInfo {
    std::string app;
    std::string stream;
    std::string session_id;
};

struct SelectedTrack {
    irs3::SourceTrackKind kind;
    irs3::SourceCodec codec;
    int clock_rate = 0;
    int channels = 0;
    int width = 0;
    int height = 0;
    int sink_stream_index = -1;
    uint32_t last_rtp = 0;
    uint64_t last_extended_timestamp = 0;
    uint64_t wrap_count = 0;
    bool seen_first_frame = false;
    std::shared_ptr<rtc::Track> track;
    std::vector<uint8_t> extradata;
};

struct PendingPacket {
    size_t track_index = 0;
    int64_t pts = 0;
    int64_t dts = 0;
    int64_t duration = 0;
    int key_frame = 0;
    std::vector<uint8_t> payload;
};

struct WhipSessionSharedState {
};

class WhipSource : public irs3::ISource {
public:
    WhipSource(irs3::SourceDescriptor descriptor, const std::shared_ptr<WhipSessionSharedState> &shared_state)
        : descriptor_(std::move(descriptor)),
          source_(std::make_shared<irs3::BufferedSource>(descriptor_)),
          shared_state_(shared_state) {
    }

    const irs3::SourceDescriptor &Descriptor() const override {
        return source_->Descriptor();
    }

    bool WaitReady(irs3::SourceFormat *out) override {
        return source_->WaitReady(out);
    }

    irs3::SourceSubscriptionPtr Subscribe() override {
        return source_->Subscribe();
    }

    void SetFormat(irs3::SourceFormat format) {
        source_->SetFormat(std::move(format));
    }

    bool PublishPacket(PendingPacket packet, irs3::SourceTrackKind track_kind) {
        irs3::SourcePacket source_packet;
        source_packet.stream_index = packet.track_index;
        source_packet.track_kind = track_kind;
        source_packet.pts = packet.pts;
        source_packet.dts = packet.dts;
        source_packet.duration = packet.duration;
        source_packet.key_frame = packet.key_frame != 0;
        source_packet.gop_start = source_packet.track_kind == irs3::SourceTrackKind::kVideo && source_packet.key_frame;
        source_packet.payload = std::make_shared<std::vector<std::uint8_t>>(std::move(packet.payload));
        return source_->PublishPacket(std::move(source_packet));
    }

    void Close() override {
        source_->Close();
    }

private:
    irs3::SourceDescriptor descriptor_;
    std::shared_ptr<irs3::BufferedSource> source_;
    std::weak_ptr<WhipSessionSharedState> shared_state_;
};

static std::string trim(const std::string &value) {
    size_t start = 0;
    size_t end = value.size();
    while (start < end && (value[start] == ' ' || value[start] == '\t' || value[start] == '\r' || value[start] == '\n')) {
        ++start;
    }
    while (end > start && (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\r' || value[end - 1] == '\n')) {
        --end;
    }
    return value.substr(start, end - start);
}

static std::string lowercase(const std::string &value) {
    std::string out = value;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

static bool read_full_request(int fd, HttpRequest *request) {
    std::string buffer;
    char chunk[4096];
    size_t header_end = std::string::npos;
    size_t content_length = 0;

    while (buffer.size() < kMaxRequestBytes) {
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            return false;
        }
        buffer.append(chunk, static_cast<size_t>(n));
        header_end = buffer.find("\r\n\r\n");
        if (header_end != std::string::npos) {
            break;
        }
    }

    if (header_end == std::string::npos) {
        return false;
    }

    std::string head = buffer.substr(0, header_end);
    std::istringstream stream(head);
    std::string line;

    if (!std::getline(stream, line)) {
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    {
        std::istringstream request_line(line);
        if (!(request_line >> request->method >> request->path)) {
            return false;
        }
    }

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string key = lowercase(trim(line.substr(0, colon)));
        std::string value = trim(line.substr(colon + 1));
        request->headers[key] = value;
        if (key == "content-length") {
            content_length = static_cast<size_t>(std::strtoul(value.c_str(), nullptr, 10));
        }
    }

    request->body = buffer.substr(header_end + 4);
    while (request->body.size() < content_length && request->body.size() < kMaxRequestBytes) {
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            return false;
        }
        request->body.append(chunk, static_cast<size_t>(n));
    }
    if (request->body.size() != content_length) {
        return false;
    }
    return true;
}

static bool write_full(int fd, const std::string &payload) {
    size_t offset = 0;
    while (offset < payload.size()) {
        ssize_t n = send(fd, payload.data() + offset, payload.size() - offset, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        offset += static_cast<size_t>(n);
    }
    return true;
}

static bool write_response(int fd, const HttpResponse &response) {
    std::ostringstream out;
    out << "HTTP/1.1 " << response.status << ' ' << response.reason << "\r\n";
    for (const auto &entry : response.headers) {
        out << entry.first << ": " << entry.second << "\r\n";
    }
    out << "Content-Length: " << response.body.size() << "\r\n";
    out << "Connection: close\r\n\r\n";
    out << response.body;
    std::string payload = out.str();
    if (payload.size() > kMaxResponseBytes) {
        return false;
    }
    return write_full(fd, payload);
}

static bool parse_path(const std::string &raw_path, PathInfo *path_info, bool *has_session_id) {
    std::string path = raw_path;
    size_t query = path.find('?');
    if (query != std::string::npos) {
        path = path.substr(0, query);
    }

    std::vector<std::string> parts;
    std::stringstream ss(path);
    std::string part;
    while (std::getline(ss, part, '/')) {
        if (!part.empty()) {
            parts.push_back(part);
        }
    }

    if (parts.size() < 3) {
        return false;
    }

    if (parts[parts.size() - 1] == "whip") {
        *has_session_id = false;
        path_info->app = parts[0];
        path_info->session_id.clear();
        path_info->stream.clear();
        for (size_t i = 1; i + 1 < parts.size(); ++i) {
            if (!path_info->stream.empty()) {
                path_info->stream.push_back('/');
            }
            path_info->stream += parts[i];
        }
        return !path_info->stream.empty();
    }

    if (parts.size() < 4 || parts[parts.size() - 2] != "whip") {
        return false;
    }

    *has_session_id = true;
    path_info->app = parts[0];
    path_info->session_id = parts.back();
    path_info->stream.clear();
    for (size_t i = 1; i + 2 < parts.size(); ++i) {
        if (!path_info->stream.empty()) {
            path_info->stream.push_back('/');
        }
        path_info->stream += parts[i];
    }
    return !path_info->stream.empty() && !path_info->session_id.empty();
}

static std::string response_location(const PathInfo &path_info, const std::string &session_id) {
    return "/" + path_info.app + "/" + path_info.stream + "/whip/" + session_id;
}

static std::optional<std::vector<uint8_t>> base64_decode(const std::string &value) {
    std::vector<uint8_t> out(value.size());
    int decoded = av_base64_decode(out.data(), value.c_str(), static_cast<int>(out.size()));
    if (decoded <= 0) {
        return std::nullopt;
    }
    out.resize(static_cast<size_t>(decoded));
    return out;
}

static std::optional<std::string> fmtp_value(const rtc::Description::Media::RtpMap &rtp_map, const std::string &key) {
    std::string prefix = key + "=";
    for (const std::string &fmtp : rtp_map.fmtps) {
        size_t start = 0;
        while (start < fmtp.size()) {
            size_t end = fmtp.find(';', start);
            if (end == std::string::npos) {
                end = fmtp.size();
            }
            std::string token = trim(fmtp.substr(start, end - start));
            if (token.rfind(prefix, 0) == 0) {
                return token.substr(prefix.size());
            }
            start = end + 1;
        }
    }
    return std::nullopt;
}

static bool build_h264_avcc(const rtc::Description::Media::RtpMap &rtp_map, std::vector<uint8_t> *extradata) {
    auto sprop = fmtp_value(rtp_map, "sprop-parameter-sets");
    if (!sprop.has_value()) {
        return false;
    }

    size_t comma = sprop->find(',');
    if (comma == std::string::npos) {
        return false;
    }

    auto sps = base64_decode(sprop->substr(0, comma));
    auto pps = base64_decode(sprop->substr(comma + 1));
    if (!sps.has_value() || !pps.has_value() || sps->size() < 4 || pps->empty()) {
        return false;
    }

    extradata->clear();
    extradata->reserve(11 + sps->size() + pps->size());
    extradata->push_back(1);
    extradata->push_back((*sps)[1]);
    extradata->push_back((*sps)[2]);
    extradata->push_back((*sps)[3]);
    extradata->push_back(0xff);
    extradata->push_back(0xe1);
    extradata->push_back(static_cast<uint8_t>((sps->size() >> 8) & 0xff));
    extradata->push_back(static_cast<uint8_t>(sps->size() & 0xff));
    extradata->insert(extradata->end(), sps->begin(), sps->end());
    extradata->push_back(1);
    extradata->push_back(static_cast<uint8_t>((pps->size() >> 8) & 0xff));
    extradata->push_back(static_cast<uint8_t>(pps->size() & 0xff));
    extradata->insert(extradata->end(), pps->begin(), pps->end());
    return true;
}

static bool build_h264_avcc_from_nalus(
    const std::vector<uint8_t> &sps,
    const std::vector<uint8_t> &pps,
    std::vector<uint8_t> *extradata
) {
    if (sps.size() < 4 || pps.empty()) {
        return false;
    }

    extradata->clear();
    extradata->reserve(11 + sps.size() + pps.size());
    extradata->push_back(1);
    extradata->push_back(sps[1]);
    extradata->push_back(sps[2]);
    extradata->push_back(sps[3]);
    extradata->push_back(0xff);
    extradata->push_back(0xe1);
    extradata->push_back(static_cast<uint8_t>((sps.size() >> 8) & 0xff));
    extradata->push_back(static_cast<uint8_t>(sps.size() & 0xff));
    extradata->insert(extradata->end(), sps.begin(), sps.end());
    extradata->push_back(1);
    extradata->push_back(static_cast<uint8_t>((pps.size() >> 8) & 0xff));
    extradata->push_back(static_cast<uint8_t>(pps.size() & 0xff));
    extradata->insert(extradata->end(), pps.begin(), pps.end());
    return true;
}

struct BitReader {
    const std::vector<uint8_t> &data;
    size_t bit_pos = 0;

    bool read_bit(uint32_t *value) {
        if (bit_pos >= data.size() * 8) {
            return false;
        }
        size_t byte_index = bit_pos / 8;
        int bit_index = 7 - static_cast<int>(bit_pos % 8);
        *value = (data[byte_index] >> bit_index) & 0x01u;
        ++bit_pos;
        return true;
    }

    bool read_bits(int count, uint32_t *value) {
        uint32_t out = 0;
        for (int i = 0; i < count; ++i) {
            uint32_t bit = 0;
            if (!read_bit(&bit)) {
                return false;
            }
            out = (out << 1) | bit;
        }
        *value = out;
        return true;
    }

    bool read_ue(uint32_t *value) {
        int leading_zero_bits = 0;
        uint32_t bit = 0;
        while (true) {
            if (!read_bit(&bit)) {
                return false;
            }
            if (bit != 0) {
                break;
            }
            ++leading_zero_bits;
        }
        if (leading_zero_bits == 0) {
            *value = 0;
            return true;
        }
        uint32_t suffix = 0;
        if (!read_bits(leading_zero_bits, &suffix)) {
            return false;
        }
        *value = ((1u << leading_zero_bits) - 1u) + suffix;
        return true;
    }

    bool read_se(int32_t *value) {
        uint32_t ue = 0;
        if (!read_ue(&ue)) {
            return false;
        }
        *value = (ue & 1u) ? static_cast<int32_t>((ue + 1u) / 2u) : -static_cast<int32_t>(ue / 2u);
        return true;
    }
};

static std::vector<uint8_t> rbsp_from_ebsp(const std::vector<uint8_t> &ebsp) {
    std::vector<uint8_t> rbsp;
    rbsp.reserve(ebsp.size());
    for (size_t i = 0; i < ebsp.size(); ++i) {
        if (i + 2 < ebsp.size() && ebsp[i] == 0x00 && ebsp[i + 1] == 0x00 && ebsp[i + 2] == 0x03) {
            rbsp.push_back(0x00);
            rbsp.push_back(0x00);
            i += 2;
            continue;
        }
        rbsp.push_back(ebsp[i]);
    }
    return rbsp;
}

static bool h264_sps_dimensions(const std::vector<uint8_t> &sps, int *width, int *height) {
    if (sps.size() < 4) {
        return false;
    }

    std::vector<uint8_t> rbsp = rbsp_from_ebsp(sps);
    BitReader reader{rbsp};
    uint32_t tmp = 0;
    int32_t stmp = 0;

    if (!reader.read_bits(8, &tmp)) { // nal header
        return false;
    }
    uint32_t profile_idc = 0;
    if (!reader.read_bits(8, &profile_idc)) {
        return false;
    }
    if (!reader.read_bits(8, &tmp)) { // constraint flags + reserved
        return false;
    }
    if (!reader.read_bits(8, &tmp)) { // level_idc
        return false;
    }
    if (!reader.read_ue(&tmp)) { // seq_parameter_set_id
        return false;
    }

    uint32_t chroma_format_idc = 1;
    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 || profile_idc == 244 ||
        profile_idc == 44 || profile_idc == 83 || profile_idc == 86 || profile_idc == 118 ||
        profile_idc == 128 || profile_idc == 138 || profile_idc == 139 || profile_idc == 134 ||
        profile_idc == 135) {
        if (!reader.read_ue(&chroma_format_idc)) {
            return false;
        }
        if (chroma_format_idc == 3) {
            if (!reader.read_bits(1, &tmp)) {
                return false;
            }
        }
        if (!reader.read_ue(&tmp)) { // bit_depth_luma_minus8
            return false;
        }
        if (!reader.read_ue(&tmp)) { // bit_depth_chroma_minus8
            return false;
        }
        if (!reader.read_bits(1, &tmp)) { // qpprime_y_zero_transform_bypass_flag
            return false;
        }
        if (!reader.read_bits(1, &tmp)) { // seq_scaling_matrix_present_flag
            return false;
        }
        if (tmp != 0) {
            return false;
        }
    }

    if (!reader.read_ue(&tmp)) { // log2_max_frame_num_minus4
        return false;
    }
    uint32_t pic_order_cnt_type = 0;
    if (!reader.read_ue(&pic_order_cnt_type)) {
        return false;
    }
    if (pic_order_cnt_type == 0) {
        if (!reader.read_ue(&tmp)) {
            return false;
        }
    } else if (pic_order_cnt_type == 1) {
        if (!reader.read_bits(1, &tmp)) {
            return false;
        }
        if (!reader.read_se(&stmp) || !reader.read_se(&stmp)) {
            return false;
        }
        if (!reader.read_ue(&tmp)) {
            return false;
        }
        for (uint32_t i = 0; i < tmp; ++i) {
            if (!reader.read_se(&stmp)) {
                return false;
            }
        }
    }
    if (!reader.read_ue(&tmp)) { // max_num_ref_frames
        return false;
    }
    if (!reader.read_bits(1, &tmp)) { // gaps_in_frame_num_value_allowed_flag
        return false;
    }

    uint32_t pic_width_in_mbs_minus1 = 0;
    uint32_t pic_height_in_map_units_minus1 = 0;
    if (!reader.read_ue(&pic_width_in_mbs_minus1) ||
        !reader.read_ue(&pic_height_in_map_units_minus1)) {
        return false;
    }

    uint32_t frame_mbs_only_flag = 0;
    if (!reader.read_bits(1, &frame_mbs_only_flag)) {
        return false;
    }
    if (frame_mbs_only_flag == 0) {
        if (!reader.read_bits(1, &tmp)) {
            return false;
        }
    }
    if (!reader.read_bits(1, &tmp)) { // direct_8x8_inference_flag
        return false;
    }

    uint32_t frame_cropping_flag = 0;
    if (!reader.read_bits(1, &frame_cropping_flag)) {
        return false;
    }

    uint32_t crop_left = 0;
    uint32_t crop_right = 0;
    uint32_t crop_top = 0;
    uint32_t crop_bottom = 0;
    if (frame_cropping_flag != 0) {
        if (!reader.read_ue(&crop_left) ||
            !reader.read_ue(&crop_right) ||
            !reader.read_ue(&crop_top) ||
            !reader.read_ue(&crop_bottom)) {
            return false;
        }
    }

    int sub_width_c = 1;
    int sub_height_c = 1;
    if (chroma_format_idc == 1) {
        sub_width_c = 2;
        sub_height_c = 2;
    } else if (chroma_format_idc == 2) {
        sub_width_c = 2;
        sub_height_c = 1;
    }

    int crop_unit_x = 1;
    int crop_unit_y = 2 - static_cast<int>(frame_mbs_only_flag);
    if (chroma_format_idc != 0) {
        crop_unit_x = sub_width_c;
        crop_unit_y = sub_height_c * (2 - static_cast<int>(frame_mbs_only_flag));
    }

    int width_pixels = static_cast<int>((pic_width_in_mbs_minus1 + 1) * 16);
    int height_pixels = static_cast<int>((pic_height_in_map_units_minus1 + 1) * 16 * (2 - frame_mbs_only_flag));
    width_pixels -= static_cast<int>((crop_left + crop_right) * crop_unit_x);
    height_pixels -= static_cast<int>((crop_top + crop_bottom) * crop_unit_y);
    if (width_pixels <= 0 || height_pixels <= 0) {
        return false;
    }

    *width = width_pixels;
    *height = height_pixels;
    return true;
}

static std::vector<uint8_t> build_opus_head(int channels) {
    std::vector<uint8_t> out(19, 0);
    const char signature[] = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'};
    std::memcpy(out.data(), signature, sizeof(signature));
    out[8] = 1;
    out[9] = static_cast<uint8_t>(channels > 0 ? channels : 2);
    out[12] = 0x80;
    out[13] = 0xbb;
    out[18] = 0;
    return out;
}

static bool next_annexb_nalu(
    const std::vector<uint8_t> &data,
    size_t *offset,
    std::vector<uint8_t> *nalu
) {
    size_t i = *offset;
    size_t start = std::string::npos;
    size_t start_code_len = 0;

    while (i + 3 < data.size()) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            start = i + 3;
            start_code_len = 3;
            break;
        }
        if (i + 4 < data.size() && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
            start = i + 4;
            start_code_len = 4;
            break;
        }
        ++i;
    }

    if (start == std::string::npos) {
        *offset = data.size();
        return false;
    }

    i = start;
    size_t end = data.size();
    while (i + 3 < data.size()) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            end = i;
            break;
        }
        if (i + 4 < data.size() && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1) {
            end = i;
            break;
        }
        ++i;
    }

    (void)start_code_len;
    *nalu = std::vector<uint8_t>(data.begin() + static_cast<std::ptrdiff_t>(start), data.begin() + static_cast<std::ptrdiff_t>(end));
    *offset = end;
    return !nalu->empty();
}

static std::vector<uint8_t> bytes_to_uint8(const rtc::binary &data) {
    std::vector<uint8_t> out(data.size());
    for (size_t i = 0; i < data.size(); ++i) {
        out[i] = std::to_integer<uint8_t>(data[i]);
    }
    return out;
}

static bool convert_h264_annexb_to_avcc(
    const std::vector<uint8_t> &annexb,
    std::vector<uint8_t> *avcc,
    std::vector<uint8_t> *sps,
    std::vector<uint8_t> *pps,
    int *key_frame
) {
    size_t offset = 0;
    std::vector<uint8_t> nalu;

    avcc->clear();
    if (sps != nullptr) {
        sps->clear();
    }
    if (pps != nullptr) {
        pps->clear();
    }
    *key_frame = 0;

    while (next_annexb_nalu(annexb, &offset, &nalu)) {
        if (nalu.empty()) {
            continue;
        }
        uint8_t nal_type = nalu[0] & 0x1f;
        if (nal_type == 7 && sps != nullptr && sps->empty()) {
            *sps = nalu;
        } else if (nal_type == 8 && pps != nullptr && pps->empty()) {
            *pps = nalu;
        } else if (nal_type == 5) {
            *key_frame = 1;
        }

        uint32_t nal_size = static_cast<uint32_t>(nalu.size());
        avcc->push_back(static_cast<uint8_t>((nal_size >> 24) & 0xff));
        avcc->push_back(static_cast<uint8_t>((nal_size >> 16) & 0xff));
        avcc->push_back(static_cast<uint8_t>((nal_size >> 8) & 0xff));
        avcc->push_back(static_cast<uint8_t>(nal_size & 0xff));
        avcc->insert(avcc->end(), nalu.begin(), nalu.end());
    }

    return !avcc->empty();
}

class WhipSession {
public:
    WhipSession(const irs3_webrtc_server_config &, const PathInfo &path_info, std::string session_id)
        : path_info_(path_info), session_id_(std::move(session_id)) {
        irs3::SourceDescriptor descriptor;
        descriptor.id = path_info_.app + "/" + path_info_.stream;
        descriptor.protocol = irs3::SourceProtocol::kWHIP;
        descriptor.app = path_info_.app;
        descriptor.stream = path_info_.stream;
        descriptor.session_id = std::strtoul(session_id_.c_str(), nullptr, 10);
        source_ = std::make_shared<WhipSource>(descriptor, shared_state_);
    }

    ~WhipSession() {
        close();
    }

    const std::string &session_id() const {
        return session_id_;
    }

    std::shared_ptr<WhipSource> source() const {
        return source_;
    }

    bool start(const std::string &offer_sdp, std::string *answer_sdp, std::string *error_text) {
        try {
            rtc::Configuration rtc_config;
            rtc_config.disableAutoNegotiation = true;
            rtc_config.forceMediaTransport = true;

            pc_ = std::make_shared<rtc::PeerConnection>(rtc_config);
            pc_->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
                std::unique_lock<std::mutex> lock(mutex_);
                if (state == rtc::PeerConnection::GatheringState::Complete) {
                    if (auto description = pc_->localDescription()) {
                        local_sdp_ = std::string(*description);
                    }
                    answer_ready_ = true;
                    cond_.notify_all();
                }
            });
            pc_->onStateChange([this](rtc::PeerConnection::State state) {
                if (state == rtc::PeerConnection::State::Closed ||
                    state == rtc::PeerConnection::State::Failed ||
                    state == rtc::PeerConnection::State::Disconnected) {
                    close();
                }
            });

            rtc::Description remote(offer_sdp, "offer");
            if (!configure_tracks(remote, error_text)) {
                return false;
            }
            if (source_config_ready()) {
                source_->SetFormat(build_source_format());
            }

            pc_->setRemoteDescription(remote);
            pc_->setLocalDescription(rtc::Description::Type::Answer);

            std::unique_lock<std::mutex> lock(mutex_);
            if (!cond_.wait_for(lock, std::chrono::seconds(10), [this]() { return answer_ready_; })) {
                *error_text = "timed out while gathering ICE candidates";
                return false;
            }
            *answer_sdp = local_sdp_;
            return true;
        } catch (const std::exception &exc) {
            *error_text = exc.what();
            return false;
        }
    }

    void close() {
        std::shared_ptr<rtc::PeerConnection> pc;

        {
            std::lock_guard<std::mutex> lock(close_mutex_);
            if (closed_) {
                return;
            }
            closed_ = true;
            pc = std::move(pc_);
        }

        if (pc != nullptr) {
            try {
                pc->close();
            } catch (...) {
            }
        }
        source_->Close();
    }

private:
    bool configure_tracks(const rtc::Description &remote, std::string *error_text) {
        std::vector<SelectedTrack> tracks;

        for (int i = 0; i < remote.mediaCount(); ++i) {
            auto media_variant = remote.media(i);
            if (!std::holds_alternative<const rtc::Description::Media *>(media_variant)) {
                continue;
            }

            const rtc::Description::Media *offered = std::get<const rtc::Description::Media *>(media_variant);
            if (offered == nullptr) {
                continue;
            }

            std::string media_type = offered->type();
            std::optional<int> selected_payload_type;
            irs3::SourceTrackKind kind;
            irs3::SourceCodec codec;
            int channels = 0;

            if (media_type == "video") {
                for (int pt : offered->payloadTypes()) {
                    const auto *rtp_map = offered->rtpMap(pt);
                    if (rtp_map != nullptr && rtp_map->format == "H264") {
                        selected_payload_type = pt;
                        kind = irs3::SourceTrackKind::kVideo;
                        codec = irs3::SourceCodec::kH264;
                        break;
                    }
                }
                if (!selected_payload_type.has_value()) {
                    *error_text = "only H264 video is supported for WHIP ingest";
                    return false;
                }
            } else if (media_type == "audio") {
                for (int pt : offered->payloadTypes()) {
                    const auto *rtp_map = offered->rtpMap(pt);
                    if (rtp_map != nullptr && rtp_map->format == "opus") {
                        selected_payload_type = pt;
                        kind = irs3::SourceTrackKind::kAudio;
                        codec = irs3::SourceCodec::kOpus;
                        channels = rtp_map->encParams.empty() ? 2 : std::max(1, std::atoi(rtp_map->encParams.c_str()));
                        break;
                    }
                }
                if (!selected_payload_type.has_value()) {
                    *error_text = "only Opus audio is supported for WHIP ingest";
                    return false;
                }
            } else {
                continue;
            }

            rtc::Description::Media local = offered->reciprocate();
            std::vector<int> payload_types = local.payloadTypes();
            for (int pt : payload_types) {
                if (pt != *selected_payload_type) {
                    local.removeRtpMap(pt);
                }
            }
            local.disableRtx();
            local.clearSSRCs();

            SelectedTrack selected;
            selected.kind = kind;
            selected.codec = codec;
            selected.channels = channels;
            selected.clock_rate = offered->rtpMap(*selected_payload_type)->clockRate;
            selected.track = pc_->addTrack(local);

            const auto *selected_map = offered->rtpMap(*selected_payload_type);
            if (codec == irs3::SourceCodec::kH264) {
                (void)build_h264_avcc(*selected_map, &selected.extradata);
                if (auto sprop = fmtp_value(*selected_map, "sprop-parameter-sets")) {
                    size_t comma = sprop->find(',');
                    if (comma != std::string::npos) {
                        auto sps = base64_decode(sprop->substr(0, comma));
                        if (sps.has_value()) {
                            (void)h264_sps_dimensions(*sps, &selected.width, &selected.height);
                        }
                    }
                }
                selected.track->setMediaHandler(std::make_shared<rtc::H264RtpDepacketizer>(
                    rtc::NalUnit::Separator::StartSequence
                ));
            } else {
                selected.extradata = build_opus_head(selected.channels);
                selected.track->setMediaHandler(std::make_shared<rtc::OpusRtpDepacketizer>());
            }
            selected.track->chainMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());

            selected.track->onFrame([this, index = tracks.size()](rtc::binary data, rtc::FrameInfo info) {
                handle_frame(index, data, info);
            });
            tracks.push_back(std::move(selected));
        }

        if (tracks.empty()) {
            *error_text = "offer contained no supported media tracks";
            return false;
        }

        tracks_ = std::move(tracks);
        for (size_t i = 0; i < tracks_.size(); ++i) {
            tracks_[i].sink_stream_index = static_cast<int>(i);
        }
        return true;
    }

    bool source_config_ready() const {
        for (const SelectedTrack &track : tracks_) {
            if (track.codec == irs3::SourceCodec::kH264 &&
                (track.extradata.empty() || track.width <= 0 || track.height <= 0)) {
                return false;
            }
        }
        return true;
    }

    irs3::SourceFormat build_source_format() const {
        irs3::SourceFormat format;
        format.payload_mode = irs3::SourcePayloadMode::kPacketized;
        for (const SelectedTrack &track : tracks_) {
            irs3::SourceTrackConfig config;
            config.kind = track.kind;
            config.codec = track.codec;
            config.clock_rate = track.clock_rate;
            config.channels = track.channels;
            config.width = track.width;
            config.height = track.height;
            config.extradata = track.extradata;
            format.tracks.push_back(std::move(config));
        }
        return format;
    }

    void handle_frame(size_t track_index, const rtc::binary &data, const rtc::FrameInfo &info) {
        std::lock_guard<std::mutex> guard(media_mutex_);
        if (track_index >= tracks_.size()) {
            return;
        }

        SelectedTrack &track = tracks_[track_index];
        uint64_t extended_timestamp = info.timestamp;
        if (track.seen_first_frame) {
            if (info.timestamp < track.last_rtp && (track.last_rtp - info.timestamp) > 0x80000000u) {
                track.wrap_count += 1ull << 32;
            }
            extended_timestamp = track.wrap_count + info.timestamp;
        } else {
            track.seen_first_frame = true;
        }

        int64_t pts = static_cast<int64_t>(extended_timestamp);
        int64_t duration = 0;
        if (track.last_extended_timestamp != 0 && extended_timestamp >= track.last_extended_timestamp) {
            duration = pts - static_cast<int64_t>(track.last_extended_timestamp);
        }
        track.last_rtp = info.timestamp;
        track.last_extended_timestamp = extended_timestamp;

        PendingPacket packet;
        packet.track_index = track_index;
        packet.pts = pts;
        packet.dts = pts;
        packet.duration = duration;

        std::vector<uint8_t> payload = bytes_to_uint8(data);
        int key_frame = 0;
        if (track.codec == irs3::SourceCodec::kH264) {
            std::vector<uint8_t> avcc_payload;
            std::vector<uint8_t> sps;
            std::vector<uint8_t> pps;
            if (!convert_h264_annexb_to_avcc(payload, &avcc_payload, &sps, &pps, &key_frame)) {
                return;
            }
            payload.swap(avcc_payload);
            if (track.extradata.empty() && !sps.empty() && !pps.empty()) {
                (void)build_h264_avcc_from_nalus(sps, pps, &track.extradata);
            }
            if ((track.width <= 0 || track.height <= 0) && !sps.empty()) {
                (void)h264_sps_dimensions(sps, &track.width, &track.height);
            }
        }

        packet.key_frame = key_frame;
        packet.payload.swap(payload);
        if (source_config_ready()) {
            source_->SetFormat(build_source_format());
        }
        if (!source_->PublishPacket(std::move(packet), track.kind)) {
            close();
        }
    }

    PathInfo path_info_;
    std::string session_id_;
    std::shared_ptr<rtc::PeerConnection> pc_;
    std::vector<SelectedTrack> tracks_;
    std::shared_ptr<WhipSessionSharedState> shared_state_ = std::make_shared<WhipSessionSharedState>();
    std::shared_ptr<WhipSource> source_;

    std::mutex mutex_;
    std::condition_variable cond_;
    bool answer_ready_ = false;
    std::string local_sdp_;

    std::mutex close_mutex_;
    std::mutex media_mutex_;
    bool closed_ = false;
};

class SessionRegistry {
public:
    std::shared_ptr<WhipSession> create(const irs3_webrtc_server_config &config, const PathInfo &path_info) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string session_id = std::to_string(++next_id_);
        auto session = std::make_shared<WhipSession>(config, path_info, session_id);
        sessions_[session_id] = session;
        return session;
    }

    std::shared_ptr<WhipSession> remove(const std::string &session_id) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            return nullptr;
        }
        auto session = it->second;
        sessions_.erase(it);
        return session;
    }

    void close_all() {
        std::map<std::string, std::shared_ptr<WhipSession>> sessions;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sessions.swap(sessions_);
        }
        for (const auto &entry : sessions) {
            entry.second->close();
        }
    }

private:
    std::mutex mutex_;
    unsigned long next_id_ = 0;
    std::map<std::string, std::shared_ptr<WhipSession>> sessions_;
};

static HttpResponse make_plain_response(int status, const std::string &reason, const std::string &body) {
    HttpResponse response;
    response.status = status;
    response.reason = reason;
    response.body = body;
    response.headers["Content-Type"] = "text/plain; charset=utf-8";
    return response;
}

static HttpResponse handle_request(
    const irs3_webrtc_server_config &config,
    SessionRegistry *registry,
    irs3::SourceQueue *source_queue,
    const HttpRequest &request
) {
    bool has_session_id = false;
    PathInfo path_info;

    if (!parse_path(request.path, &path_info, &has_session_id)) {
        return make_plain_response(404, "Not Found", "not found\n");
    }

    if (request.method == "OPTIONS" && !has_session_id) {
        HttpResponse response;
        response.status = 204;
        response.reason = "No Content";
        response.headers["Allow"] = "OPTIONS, POST, DELETE";
        response.headers["Accept-Post"] = "application/sdp";
        response.headers["Access-Control-Allow-Methods"] = "OPTIONS, POST, DELETE";
        response.headers["Access-Control-Allow-Headers"] = "Content-Type";
        return response;
    }

    if (request.method == "POST" && !has_session_id) {
        auto content_type_it = request.headers.find("content-type");
        if (content_type_it == request.headers.end() ||
            lowercase(trim(content_type_it->second)).find("application/sdp") != 0) {
            return make_plain_response(400, "Bad Request", "invalid Content-Type\n");
        }

        auto session = registry->create(config, path_info);
        std::string answer_sdp;
        std::string error_text;
        if (!session->start(request.body, &answer_sdp, &error_text)) {
            registry->remove(session->session_id());
            session->close();
            std::fprintf(stderr, "argus: WHIP session rejected app=%s stream=%s reason=%s\n",
                path_info.app.c_str(), path_info.stream.c_str(), error_text.c_str());
            return make_plain_response(400, "Bad Request", error_text + "\n");
        }

        HttpResponse response;
        response.status = 201;
        response.reason = "Created";
        response.body = answer_sdp;
        response.headers["Content-Type"] = "application/sdp";
        response.headers["Location"] = response_location(path_info, session->session_id());
        response.headers["Accept-Post"] = "application/sdp";
        source_queue->Push(session->source());
        return response;
    }

    if (request.method == "DELETE" && has_session_id) {
        auto session = registry->remove(path_info.session_id);
        if (session == nullptr) {
            return make_plain_response(404, "Not Found", "session not found\n");
        }
        session->close();
        HttpResponse response;
        response.status = 204;
        response.reason = "No Content";
        return response;
    }

    return make_plain_response(405, "Method Not Allowed", "method not allowed\n");
}

} // namespace

class WhipServer : public irs3::IServer {
public:
    explicit WhipServer(const irs3_webrtc_server_config &config) : config_(config) {
    }

    int Start() override {
        rtc::InitLogger(rtc::LogLevel::Warning);
        rtc_initialized_ = true;

        int listener_fd = create_listener();
        if (listener_fd < 0) {
            Close();
            cleanup_rtc();
            return 1;
        }
        listener_fd_.store(listener_fd);

        std::fprintf(stderr, "argus: WHIP listening on http://%s:%u\n", config_.bind_host, static_cast<unsigned>(config_.port));

        int result = 0;
        while (!closing_.load()) {
            int client_fd = accept(listener_fd_.load(), nullptr, nullptr);
            if (client_fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (closing_.load() || errno == EBADF || errno == EINVAL) {
                    break;
                }
                std::perror("accept");
                result = 1;
                break;
            }

            HttpRequest request;
            HttpResponse response;
            if (!read_full_request(client_fd, &request)) {
                response = make_plain_response(400, "Bad Request", "failed to parse request\n");
            } else {
                response = handle_request(config_, &registry_, &source_queue_, request);
            }
            (void)write_response(client_fd, response);
            close(client_fd);
        }

        Close();
        cleanup_rtc();
        return result;
    }

    void Close() override {
        if (closing_.exchange(true)) {
            return;
        }

        source_queue_.Close();

        int listener_fd = listener_fd_.exchange(-1);
        if (listener_fd >= 0) {
            shutdown(listener_fd, SHUT_RDWR);
            close(listener_fd);
        }

        registry_.close_all();
    }

    irs3::SourcePtr NextSource() override {
        return source_queue_.Pop();
    }

private:
    int create_listener() const {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            std::perror("socket");
            return -1;
        }

        int opt = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
            std::perror("setsockopt");
            close(fd);
            return -1;
        }

        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(config_.port);
        if (inet_pton(AF_INET, config_.bind_host, &addr.sin_addr) != 1) {
            std::fprintf(stderr, "invalid bind host: %s\n", config_.bind_host);
            close(fd);
            return -1;
        }

        if (bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
            std::perror("bind");
            close(fd);
            return -1;
        }

        if (listen(fd, 32) != 0) {
            std::perror("listen");
            close(fd);
            return -1;
        }

        return fd;
    }

    void cleanup_rtc() {
        if (!rtc_initialized_) {
            return;
        }
        rtc_initialized_ = false;
        rtc::Cleanup();
    }

    irs3_webrtc_server_config config_{};
    SessionRegistry registry_;
    irs3::SourceQueue source_queue_;
    std::atomic<bool> closing_{false};
    std::atomic<int> listener_fd_{-1};
    bool rtc_initialized_ = false;
};

extern "C" int irs3_webrtc_server_run(const irs3_webrtc_server_config *config) {
    return WhipServer(*config).Start();
}

std::unique_ptr<irs3::IServer> irs3_create_webrtc_server(const irs3_webrtc_server_config &config) {
    return std::make_unique<WhipServer>(config);
}
