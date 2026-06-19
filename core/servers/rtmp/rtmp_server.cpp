#include "core/servers/rtmp/rtmp_server.h"

#include "core/sources/buffered_source.h"
#include "core/sources/packet_typing.h"
#include "core/sources/source_queue.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

class RtmpSource : public irs3::ISource {
public:
    explicit RtmpSource(unsigned long session_id, int client_fd)
        : descriptor_{
              "",
              irs3::SourceProtocol::kRTMP,
              "",
              "",
              session_id,
          },
          client_fd_(client_fd),
          source_(std::make_shared<irs3::BufferedSource>(descriptor_)) {
        irs3::SourceFormat format;
        format.payload_mode = irs3::SourcePayloadMode::kFlvTag;
        source_->SetFormat(format);
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

    irs3::SourceSubscriptionPtr Subscribe(const irs3::SubscriptionFilter &filter) override {
        return source_->Subscribe(filter);
    }

    void SetStreamPath(const char *app, const char *stream_name) {
        descriptor_.app = app != nullptr ? app : "";
        descriptor_.stream = stream_name != nullptr ? stream_name : "";
        descriptor_.id = descriptor_.app + "/" + descriptor_.stream;
        source_->SetDescriptor(descriptor_);
    }

    int PublishFlvTag(std::uint8_t type_id, std::uint32_t timestamp_ms, const std::uint8_t *payload, size_t payload_len) {
        irs3::SourcePacket packet;
        packet.stream_index = 0;
        packet.tag_type = type_id;
        packet.timestamp_ms = timestamp_ms;
        packet.track_kind = irs3::SourceTrackKind::kData;
        packet.payload = std::make_shared<std::vector<std::uint8_t>>(payload, payload + payload_len);
        irs3::AssignRtmpFlvPacketTyping(&packet, type_id);

        if (type_id == 8) {
            if (is_audio_config_tag(payload, payload_len)) {
                audio_config_packet_ = packet;
                update_resume_prefix(packet.stream_id);
                return 0;
            }
        } else if (type_id == 9) {
            if (is_video_config_tag(payload, payload_len)) {
                video_config_packet_ = packet;
                update_resume_prefix(packet.stream_id);
                update_video_format(payload, payload_len);
                return 0;
            }
            packet.key_frame = is_video_keyframe_tag(payload, payload_len);
            packet.gop_start = packet.key_frame;
        } else if (type_id == 18) {
            metadata_packet_ = packet;
            update_resume_prefix(packet.stream_id);
            return 0;
        }

        return source_->PublishPacket(std::move(packet)) ? 0 : -1;
    }

    void MarkClosed() {
        std::lock_guard<std::mutex> lock(mutex_);
        client_fd_ = -1;
        source_->Close();
    }

    void Close() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (client_fd_ >= 0) {
            shutdown(client_fd_, SHUT_RDWR);
        }
    }

private:
    static bool is_video_config_tag(const std::uint8_t *payload, size_t payload_len) {
        return payload != nullptr && payload_len >= 2 && (payload[0] & 0x0f) == 7 && payload[1] == 0;
    }

    static bool is_video_keyframe_tag(const std::uint8_t *payload, size_t payload_len) {
        return payload != nullptr && payload_len >= 2 && ((payload[0] >> 4) == 1) && ((payload[0] & 0x0f) == 7) && payload[1] == 1;
    }

    static bool is_audio_config_tag(const std::uint8_t *payload, size_t payload_len) {
        return payload != nullptr && payload_len >= 2 && ((payload[0] >> 4) == 10) && payload[1] == 0;
    }

    void update_resume_prefix(const std::string &stream_id) {
        std::vector<irs3::SourcePacket> prefix_packets;
        if (stream_id == "text/metadata" && metadata_packet_.has_value()) {
            prefix_packets.push_back(*metadata_packet_);
        }
        if (stream_id == "voice/main" && audio_config_packet_.has_value()) {
            prefix_packets.push_back(*audio_config_packet_);
        }
        if (stream_id == "video/main" && video_config_packet_.has_value()) {
            prefix_packets.push_back(*video_config_packet_);
        }
        if (!prefix_packets.empty()) {
            source_->SetResumePrefixPackets(stream_id, std::move(prefix_packets));
        }
    }

    void update_video_format(const std::uint8_t *payload, size_t payload_len) {
        if (payload == nullptr || payload_len <= 2) {
            return;
        }

        std::size_t extradata_offset = 2;
        if (payload_len >= 6 && payload[1] == 0 && payload[5] == 0x01) {
            extradata_offset = 5;
        }
        if (payload_len <= extradata_offset) {
            return;
        }

        irs3::SourceFormat format;
        format.payload_mode = irs3::SourcePayloadMode::kFlvTag;
        irs3::SourceTrackConfig video_track;
        video_track.kind = irs3::SourceTrackKind::kVideo;
        video_track.codec = irs3::SourceCodec::kH264;
        video_track.extradata.assign(payload + extradata_offset, payload + payload_len);
        format.tracks.push_back(std::move(video_track));
        source_->SetFormat(std::move(format));
    }

    irs3::SourceDescriptor descriptor_;
    std::mutex mutex_;
    int client_fd_ = -1;
    std::shared_ptr<irs3::BufferedSource> source_;
    std::optional<irs3::SourcePacket> metadata_packet_;
    std::optional<irs3::SourcePacket> audio_config_packet_;
    std::optional<irs3::SourcePacket> video_config_packet_;
};

class RtmpServer : public irs3::IServer {
public:
    explicit RtmpServer(const irs3_rtmp_server_config &config) : config_(config) {}

    int Start() override {
        int listener_fd = create_listener();
        if (listener_fd < 0) {
            Close();
            return 1;
        }
        listener_fd_.store(listener_fd);

        std::fprintf(stderr, "argus: listening on %s:%u\n", config_.bind_host, static_cast<unsigned>(config_.port));

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

            unsigned long session_id = ++session_id_;
            auto source = std::make_shared<RtmpSource>(session_id, client_fd);
            {
                std::lock_guard<std::mutex> lock(sources_mutex_);
                sources_[session_id] = source;
            }

            auto args = std::make_unique<irs3_rtmp_session_args>();
            args->client_fd = client_fd;
            args->session_id = session_id;
            args->config = config_;
            args->observer_context = this;
            args->on_publish = &RtmpServer::OnPublishThunk;
            args->on_close = &RtmpServer::OnCloseThunk;
            args->on_media = &RtmpServer::OnMediaThunk;

            try {
                std::thread worker([](irs3_rtmp_session_args *thread_args) {
                    irs3_rtmp_session_thread_main(thread_args);
                }, args.release());
                worker.detach();
            } catch (...) {
                source->MarkClosed();
                {
                    std::lock_guard<std::mutex> lock(sources_mutex_);
                    sources_.erase(session_id);
                }
                close(client_fd);
            }
        }

        Close();
        int owned_listener_fd = listener_fd_.exchange(-1);
        if (owned_listener_fd >= 0) {
            close(owned_listener_fd);
        }
        return result;
    }

    void Close() override {
        if (closing_.exchange(true)) {
            return;
        }

        source_queue_.Close();

        int listener_fd = listener_fd_.exchange(-1);
        if (listener_fd >= 0) {
            close(listener_fd);
        }

        std::map<unsigned long, std::shared_ptr<RtmpSource>> sources;
        {
            std::lock_guard<std::mutex> lock(sources_mutex_);
            sources = sources_;
        }
        for (const auto &entry : sources) {
            entry.second->Close();
        }
    }

    irs3::SourcePtr NextSource() override {
        return source_queue_.Pop();
    }

private:
    static void OnPublishThunk(
        void *observer_context,
        unsigned long session_id,
        int client_fd,
        const char *app,
        const char *stream_name
    ) {
        (void)client_fd;
        static_cast<RtmpServer *>(observer_context)->OnPublish(session_id, app, stream_name);
    }

    static void OnCloseThunk(void *observer_context, unsigned long session_id, int client_fd) {
        (void)client_fd;
        static_cast<RtmpServer *>(observer_context)->OnClose(session_id);
    }

    static int OnMediaThunk(
        void *observer_context,
        unsigned long session_id,
        std::uint8_t type_id,
        std::uint32_t timestamp_ms,
        const std::uint8_t *payload,
        size_t payload_len
    ) {
        return static_cast<RtmpServer *>(observer_context)->OnMedia(session_id, type_id, timestamp_ms, payload, payload_len);
    }

    void OnPublish(unsigned long session_id, const char *app, const char *stream_name) {
        std::shared_ptr<RtmpSource> source;
        {
            std::lock_guard<std::mutex> lock(sources_mutex_);
            auto it = sources_.find(session_id);
            if (it == sources_.end()) {
                return;
            }
            source = it->second;
        }

        source->SetStreamPath(app, stream_name);
        source_queue_.Push(source);
    }

    void OnClose(unsigned long session_id) {
        std::shared_ptr<RtmpSource> source;
        {
            std::lock_guard<std::mutex> lock(sources_mutex_);
            auto it = sources_.find(session_id);
            if (it == sources_.end()) {
                return;
            }
            source = it->second;
            sources_.erase(it);
        }

        source->MarkClosed();
    }

    int OnMedia(unsigned long session_id, std::uint8_t type_id, std::uint32_t timestamp_ms, const std::uint8_t *payload, size_t payload_len) {
        std::shared_ptr<RtmpSource> source;
        {
            std::lock_guard<std::mutex> lock(sources_mutex_);
            auto it = sources_.find(session_id);
            if (it == sources_.end()) {
                return -1;
            }
            source = it->second;
        }
        return source->PublishFlvTag(type_id, timestamp_ms, payload, payload_len);
    }

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

    irs3_rtmp_server_config config_;
    irs3::SourceQueue source_queue_;
    std::atomic<bool> closing_{false};
    std::atomic<int> listener_fd_{-1};
    std::mutex sources_mutex_;
    std::map<unsigned long, std::shared_ptr<RtmpSource>> sources_;
    unsigned long session_id_ = 0;
};

} // namespace

extern "C" int irs3_rtmp_server_run(const irs3_rtmp_server_config *config) {
    return RtmpServer(*config).Start();
}

std::unique_ptr<irs3::IServer> irs3_create_rtmp_server(const irs3_rtmp_server_config &config) {
    return std::make_unique<RtmpServer>(config);
}
