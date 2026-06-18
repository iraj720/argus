extern "C" {
#include "core/servers/rtmp/rtmp_session.h"

#include "core/servers/rtmp/rtmp_amf0.h"
#include "core/servers/rtmp/rtmp_chunk.h"
#include "core/servers/rtmp/rtmp_handshake.h"
}

#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

namespace {

class MessageGuard {
public:
    MessageGuard() = default;
    ~MessageGuard() {
        irs3_rtmp_message_free(&message_);
    }

    MessageGuard(const MessageGuard &) = delete;
    MessageGuard &operator=(const MessageGuard &) = delete;

    irs3_rtmp_message *get() {
        return &message_;
    }

private:
    irs3_rtmp_message message_{};
};

class RtmpSession {
public:
    explicit RtmpSession(const irs3_rtmp_session_args &args)
        : fd_(args.client_fd),
          session_id_(args.session_id),
          observer_context_(args.observer_context),
          on_publish_(args.on_publish),
          on_close_(args.on_close),
          on_media_(args.on_media) {
        irs3_rtmp_chunk_reader_init(&reader_);
    }

    ~RtmpSession() {
        notify_close();
        irs3_rtmp_chunk_reader_free(&reader_);
        if (fd_ >= 0) {
            close(fd_);
        }
        std::fprintf(stderr, "argus: session=%lu closed app=%s stream=%s\n",
            session_id_,
            app_[0] ? app_.data() : "-",
            stream_name_[0] ? stream_name_.data() : "-");
    }

    int run() {
        std::fprintf(stderr, "argus: session=%lu accepted\n", session_id_);
        if (irs3_rtmp_perform_handshake(fd_) != 0) {
            return 1;
        }

        while (true) {
            MessageGuard message;
            if (irs3_rtmp_read_message(fd_, &reader_, message.get()) != 0) {
                break;
            }
            if (dispatch(*message.get()) != 0) {
                break;
            }
        }

        return 0;
    }

private:
    static int send_full(int fd, const void *buf, size_t len) {
        const std::uint8_t *ptr = static_cast<const std::uint8_t *>(buf);
        size_t offset = 0;
        while (offset < len) {
            ssize_t n = send(fd, ptr + offset, len - offset, 0);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                return -1;
            }
            offset += static_cast<size_t>(n);
        }
        return 0;
    }

    static void write_be32(std::uint8_t *dst, std::uint32_t value) {
        dst[0] = static_cast<std::uint8_t>((value >> 24) & 0xff);
        dst[1] = static_cast<std::uint8_t>((value >> 16) & 0xff);
        dst[2] = static_cast<std::uint8_t>((value >> 8) & 0xff);
        dst[3] = static_cast<std::uint8_t>(value & 0xff);
    }

    static void write_le32(std::uint8_t *dst, std::uint32_t value) {
        dst[0] = static_cast<std::uint8_t>(value & 0xff);
        dst[1] = static_cast<std::uint8_t>((value >> 8) & 0xff);
        dst[2] = static_cast<std::uint8_t>((value >> 16) & 0xff);
        dst[3] = static_cast<std::uint8_t>((value >> 24) & 0xff);
    }

    static void trim_slashes(char *value) {
        size_t len = std::strlen(value);
        while (len > 0 && value[len - 1] == '/') {
            value[len - 1] = '\0';
            --len;
        }
        while (*value == '/') {
            std::memmove(value, value + 1, std::strlen(value));
        }
    }

    template <size_t N>
    static void copy_trimmed(std::array<char, N> *dst, const char *src) {
        std::snprintf(dst->data(), dst->size(), "%s", src);
        trim_slashes(dst->data());
    }

    void notify_publish() {
        if (on_publish_ == nullptr) {
            return;
        }
        on_publish_(observer_context_, session_id_, fd_, app_.data(), stream_name_.data());
    }

    void notify_close() {
        if (close_notified_ || on_close_ == nullptr) {
            return;
        }
        close_notified_ = true;
        on_close_(observer_context_, session_id_, fd_);
    }

    int send_message(
        std::uint32_t csid,
        std::uint8_t type_id,
        std::uint32_t message_stream_id,
        std::uint32_t timestamp_ms,
        const std::uint8_t *payload,
        size_t payload_len
    ) {
        std::uint8_t header[18];
        size_t header_len = 12;
        size_t offset = 0;

        header[0] = static_cast<std::uint8_t>(csid & 0x3f);
        if (timestamp_ms >= 0x00ffffffu) {
            header[1] = 0xff;
            header[2] = 0xff;
            header[3] = 0xff;
        } else {
            header[1] = static_cast<std::uint8_t>((timestamp_ms >> 16) & 0xff);
            header[2] = static_cast<std::uint8_t>((timestamp_ms >> 8) & 0xff);
            header[3] = static_cast<std::uint8_t>(timestamp_ms & 0xff);
        }
        header[4] = static_cast<std::uint8_t>((payload_len >> 16) & 0xff);
        header[5] = static_cast<std::uint8_t>((payload_len >> 8) & 0xff);
        header[6] = static_cast<std::uint8_t>(payload_len & 0xff);
        header[7] = type_id;
        write_le32(header + 8, message_stream_id);
        if (timestamp_ms >= 0x00ffffffu) {
            write_be32(header + 12, timestamp_ms);
            header_len += 4;
        }

        if (send_full(fd_, header, header_len) != 0) {
            return -1;
        }

        while (offset < payload_len) {
            size_t chunk_len = payload_len - offset;
            if (chunk_len > out_chunk_size_) {
                chunk_len = out_chunk_size_;
            }
            if (send_full(fd_, payload + offset, chunk_len) != 0) {
                return -1;
            }
            offset += chunk_len;
            if (offset < payload_len) {
                std::uint8_t cont[5];
                size_t cont_len = 1;
                cont[0] = static_cast<std::uint8_t>(0xc0 | (csid & 0x3f));
                if (timestamp_ms >= 0x00ffffffu) {
                    write_be32(cont + 1, timestamp_ms);
                    cont_len += 4;
                }
                if (send_full(fd_, cont, cont_len) != 0) {
                    return -1;
                }
            }
        }

        return 0;
    }

    int send_window_ack(std::uint32_t size) {
        std::uint8_t payload[4];
        write_be32(payload, size);
        return send_message(2, 5, 0, 0, payload, sizeof(payload));
    }

    int send_set_peer_bandwidth(std::uint32_t size) {
        std::uint8_t payload[5];
        write_be32(payload, size);
        payload[4] = 2;
        return send_message(2, 6, 0, 0, payload, sizeof(payload));
    }

    int send_chunk_size(std::uint32_t size) {
        std::uint8_t payload[4];
        write_be32(payload, size);
        return send_message(2, 1, 0, 0, payload, sizeof(payload));
    }

    int send_stream_begin(std::uint32_t stream_id) {
        std::uint8_t payload[6];
        payload[0] = 0x00;
        payload[1] = 0x00;
        write_be32(payload + 2, stream_id);
        return send_message(2, 4, 0, 0, payload, sizeof(payload));
    }

    int send_connect_result(double txid) {
        std::uint8_t payload[512];
        size_t n = 0;
        n += irs3_amf0_write_string(payload + n, "_result");
        n += irs3_amf0_write_number(payload + n, txid);
        n += irs3_amf0_write_object_start(payload + n);
        n += irs3_amf0_write_named_string(payload + n, "fmsVer", "FMS/3,5,7,7009");
        n += irs3_amf0_write_named_number(payload + n, "capabilities", 31.0);
        n += irs3_amf0_write_object_end(payload + n);
        n += irs3_amf0_write_object_start(payload + n);
        n += irs3_amf0_write_named_string(payload + n, "level", "status");
        n += irs3_amf0_write_named_string(payload + n, "code", "NetConnection.Connect.Success");
        n += irs3_amf0_write_named_string(payload + n, "description", "Connection succeeded.");
        n += irs3_amf0_write_named_number(payload + n, "objectEncoding", 0.0);
        n += irs3_amf0_write_object_end(payload + n);
        return send_message(3, 20, 0, 0, payload, n);
    }

    int send_create_stream_result(double txid, std::uint32_t stream_id) {
        std::uint8_t payload[128];
        size_t n = 0;
        n += irs3_amf0_write_string(payload + n, "_result");
        n += irs3_amf0_write_number(payload + n, txid);
        n += irs3_amf0_write_null(payload + n);
        n += irs3_amf0_write_number(payload + n, static_cast<double>(stream_id));
        return send_message(3, 20, 0, 0, payload, n);
    }

    int send_simple_status(
        std::uint32_t message_stream_id,
        const char *name,
        const char *level,
        const char *code,
        const char *description
    ) {
        std::uint8_t payload[512];
        size_t n = 0;
        n += irs3_amf0_write_string(payload + n, name);
        n += irs3_amf0_write_number(payload + n, 0.0);
        n += irs3_amf0_write_null(payload + n);
        n += irs3_amf0_write_object_start(payload + n);
        n += irs3_amf0_write_named_string(payload + n, "level", level);
        n += irs3_amf0_write_named_string(payload + n, "code", code);
        n += irs3_amf0_write_named_string(payload + n, "description", description);
        n += irs3_amf0_write_object_end(payload + n);
        return send_message(5, 20, message_stream_id, 0, payload, n);
    }

    int send_publish_start() {
        return send_simple_status(
            publish_stream_id_,
            "onStatus",
            "status",
            "NetStream.Publish.Start",
            "Start publishing."
        );
    }

    int send_on_fcpublish() {
        return send_simple_status(
            0,
            "onFCPublish",
            "status",
            "NetStream.Publish.Start",
            "FCPublish accepted."
        );
    }

    int dispatch(const irs3_rtmp_message &message) {
        if (message.type_id == 20 || message.type_id == 17) {
            return handle_command(message);
        }
        if (message.type_id == 1 || message.type_id == 2 || message.type_id == 3 ||
            message.type_id == 5 || message.type_id == 6) {
            return handle_control(message);
        }
        return handle_media(message);
    }

    int handle_command(const irs3_rtmp_message &message) {
        irs3_amf0_reader reader;
        irs3_amf0_command command;
        irs3_amf0_reader_init(&reader, message.payload, message.payload_len);
        if (irs3_amf0_parse_command(&reader, &command) != 0) {
            return -1;
        }

        if (std::strcmp(command.name, "connect") == 0) {
            if (command.app[0] != '\0') {
                copy_trimmed(&app_, command.app);
            }
            connected_ = true;
            if (send_window_ack(5000000) != 0 ||
                send_set_peer_bandwidth(5000000) != 0 ||
                send_chunk_size(4096) != 0) {
                return -1;
            }
            out_chunk_size_ = 4096;
            return send_connect_result(command.transaction_id);
        }

        if (std::strcmp(command.name, "releaseStream") == 0) {
            if (command.stream_name[0] != '\0') {
                copy_trimmed(&stream_name_, command.stream_name);
            }
            return 0;
        }

        if (std::strcmp(command.name, "FCPublish") == 0) {
            if (command.stream_name[0] != '\0') {
                copy_trimmed(&stream_name_, command.stream_name);
            }
            return send_on_fcpublish();
        }

        if (std::strcmp(command.name, "createStream") == 0) {
            stream_created_ = true;
            publish_stream_id_ = 1;
            return send_create_stream_result(command.transaction_id, publish_stream_id_);
        }

        if (std::strcmp(command.name, "publish") == 0) {
            if (!connected_ || !stream_created_) {
                return -1;
            }
            if (command.stream_name[0] != '\0') {
                copy_trimmed(&stream_name_, command.stream_name);
            }
            if (app_[0] == '\0') {
                copy_trimmed(&app_, "live");
            }
            if (stream_name_[0] == '\0') {
                copy_trimmed(&stream_name_, "stream");
            }
            publishing_ = true;
            notify_publish();
            if (send_stream_begin(publish_stream_id_) != 0) {
                return -1;
            }
            return send_publish_start();
        }

        return 0;
    }

    int handle_control(const irs3_rtmp_message &message) {
        if (message.type_id == 1 && message.payload_len >= 4) {
            std::uint32_t size = (static_cast<std::uint32_t>(message.payload[0]) << 24) |
                                 (static_cast<std::uint32_t>(message.payload[1]) << 16) |
                                 (static_cast<std::uint32_t>(message.payload[2]) << 8) |
                                 static_cast<std::uint32_t>(message.payload[3]);
            if (size >= 128 && size <= (1u << 20)) {
                reader_.in_chunk_size = size;
            }
        }
        return 0;
    }

    int handle_media(const irs3_rtmp_message &message) {
        if (!publishing_ || on_media_ == nullptr) {
            return 0;
        }
        if (message.type_id != 8 && message.type_id != 9 && message.type_id != 18) {
            return 0;
        }
        if (on_media_(observer_context_, session_id_, message.type_id, message.timestamp_ms, message.payload, message.payload_len) != 0) {
            std::fprintf(stderr, "argus: session=%lu sink write failed type=%u ts=%u len=%zu\n",
                session_id_, message.type_id, message.timestamp_ms, message.payload_len);
            return -1;
        }
        return 0;
    }

    int fd_ = -1;
    unsigned long session_id_ = 0;
    void *observer_context_ = nullptr;
    void (*on_publish_)(void *, unsigned long, int, const char *, const char *) = nullptr;
    void (*on_close_)(void *, unsigned long, int) = nullptr;
    int (*on_media_)(void *, unsigned long, std::uint8_t, std::uint32_t, const std::uint8_t *, size_t) = nullptr;
    bool close_notified_ = false;
    std::uint32_t out_chunk_size_ = 128;
    bool connected_ = false;
    bool stream_created_ = false;
    bool publishing_ = false;
    std::array<char, 256> app_{};
    std::array<char, 256> stream_name_{};
    std::uint32_t publish_stream_id_ = 0;
    irs3_rtmp_chunk_reader reader_{};
};

} // namespace

extern "C" void *irs3_rtmp_session_thread_main(void *opaque) {
    std::unique_ptr<irs3_rtmp_session_args> args(static_cast<irs3_rtmp_session_args *>(opaque));
    RtmpSession session(*args);
    session.run();
    return nullptr;
}
