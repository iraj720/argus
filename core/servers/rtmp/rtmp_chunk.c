#include "core/servers/rtmp/rtmp_chunk.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int read_full(int fd, void *buf, size_t len) {
    uint8_t *ptr = (uint8_t *)buf;
    size_t offset = 0;
    while (offset < len) {
        ssize_t n = recv(fd, ptr + offset, len - offset, 0);
        if (n == 0) {
            return -1;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        offset += (size_t)n;
    }
    return 0;
}

static uint32_t read_be24(const uint8_t *src) {
    return ((uint32_t)src[0] << 16) | ((uint32_t)src[1] << 8) | (uint32_t)src[2];
}

static uint32_t read_le32(const uint8_t *src) {
    return ((uint32_t)src[0]) |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) |
           ((uint32_t)src[3] << 24);
}

void irs3_rtmp_chunk_reader_init(irs3_rtmp_chunk_reader *reader) {
    memset(reader, 0, sizeof(*reader));
    reader->in_chunk_size = 128;
}

void irs3_rtmp_chunk_reader_free(irs3_rtmp_chunk_reader *reader) {
    for (size_t i = 0; i < IRS3_RTMP_MAX_CHUNK_STREAMS; ++i) {
        free(reader->streams[i].message);
        reader->streams[i].message = NULL;
        reader->streams[i].message_capacity = 0;
    }
}

static int ensure_capacity(irs3_rtmp_chunk_stream *stream, uint32_t message_length) {
    if (stream->message_capacity >= message_length) {
        return 0;
    }
    uint8_t *next = (uint8_t *)realloc(stream->message, message_length);
    if (next == NULL) {
        return -1;
    }
    stream->message = next;
    stream->message_capacity = message_length;
    return 0;
}

static int parse_basic_header(int fd, uint8_t *fmt, uint32_t *csid) {
    uint8_t first = 0;
    if (read_full(fd, &first, 1) != 0) {
        return -1;
    }

    *fmt = (uint8_t)(first >> 6);
    uint8_t raw_csid = (uint8_t)(first & 0x3f);
    if (raw_csid == 0) {
        uint8_t second = 0;
        if (read_full(fd, &second, 1) != 0) {
            return -1;
        }
        *csid = (uint32_t)second + 64;
    } else if (raw_csid == 1) {
        uint8_t ext[2];
        if (read_full(fd, ext, 2) != 0) {
            return -1;
        }
        *csid = (uint32_t)ext[0] + ((uint32_t)ext[1] * 256) + 64;
    } else {
        *csid = raw_csid;
    }
    if (*csid >= IRS3_RTMP_MAX_CHUNK_STREAMS) {
        return -1;
    }
    return 0;
}

int irs3_rtmp_read_message(int fd, irs3_rtmp_chunk_reader *reader, irs3_rtmp_message *message) {
    uint8_t fmt = 0;
    uint32_t csid = 0;
    uint8_t header[11];
    uint8_t ext_ts[4];
    irs3_rtmp_chunk_stream *stream = NULL;
    memset(message, 0, sizeof(*message));

    while (1) {
        if (parse_basic_header(fd, &fmt, &csid) != 0) {
            return -1;
        }
        stream = &reader->streams[csid];

        size_t mh_size = 0;
        switch (fmt) {
            case 0: mh_size = 11; break;
            case 1: mh_size = 7; break;
            case 2: mh_size = 3; break;
            case 3: mh_size = 0; break;
            default: return -1;
        }

        if (mh_size > 0 && read_full(fd, header, mh_size) != 0) {
            return -1;
        }

        uint32_t header_ts = 0;
        if (fmt <= 2 && mh_size >= 3) {
            header_ts = read_be24(header);
        }

        if (fmt == 0) {
            stream->timestamp = header_ts;
            stream->timestamp_delta = 0;
            stream->message_length = read_be24(header + 3);
            stream->message_type_id = header[6];
            stream->message_stream_id = read_le32(header + 7);
            stream->bytes_read = 0;
            stream->initialized = 1;
            if (ensure_capacity(stream, stream->message_length) != 0) {
                return -1;
            }
        } else if (fmt == 1) {
            if (!stream->initialized) {
                return -1;
            }
            stream->timestamp_delta = header_ts;
            stream->timestamp += stream->timestamp_delta;
            stream->message_length = read_be24(header + 3);
            stream->message_type_id = header[6];
            stream->bytes_read = 0;
            if (ensure_capacity(stream, stream->message_length) != 0) {
                return -1;
            }
        } else if (fmt == 2) {
            if (!stream->initialized) {
                return -1;
            }
            stream->timestamp_delta = header_ts;
            stream->timestamp += stream->timestamp_delta;
            stream->bytes_read = 0;
        } else {
            if (!stream->initialized) {
                return -1;
            }
            if (stream->bytes_read >= stream->message_length) {
                stream->bytes_read = 0;
                stream->timestamp += stream->timestamp_delta;
            }
        }

        if ((fmt == 0 || fmt == 1 || fmt == 2) && header_ts == 0xffffff) {
            if (read_full(fd, ext_ts, 4) != 0) {
                return -1;
            }
            uint32_t value = ((uint32_t)ext_ts[0] << 24) |
                             ((uint32_t)ext_ts[1] << 16) |
                             ((uint32_t)ext_ts[2] << 8) |
                             (uint32_t)ext_ts[3];
            if (fmt == 0) {
                stream->timestamp = value;
            } else if (fmt == 1 || fmt == 2) {
                if (stream->bytes_read == 0) {
                    stream->timestamp = value;
                }
            }
        } else if (fmt == 3 && (stream->timestamp == 0xffffff || stream->timestamp_delta == 0xffffff)) {
            if (read_full(fd, ext_ts, 4) != 0) {
                return -1;
            }
        }

        size_t remaining = stream->message_length - stream->bytes_read;
        size_t to_read = remaining < reader->in_chunk_size ? remaining : reader->in_chunk_size;
        if (to_read > 0 && read_full(fd, stream->message + stream->bytes_read, to_read) != 0) {
            return -1;
        }
        stream->bytes_read += to_read;

        if (stream->bytes_read < stream->message_length) {
            continue;
        }

        message->type_id = stream->message_type_id;
        message->timestamp_ms = stream->timestamp;
        message->message_stream_id = stream->message_stream_id;
        message->payload_len = stream->message_length;
        message->payload = (uint8_t *)malloc(stream->message_length);
        if (message->payload == NULL) {
            return -1;
        }
        memcpy(message->payload, stream->message, stream->message_length);
        return 0;
    }
}

void irs3_rtmp_message_free(irs3_rtmp_message *message) {
    free(message->payload);
    memset(message, 0, sizeof(*message));
}
