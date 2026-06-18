#ifndef ARGUS_CORE_SERVERS_RTMP_CHUNK_H
#define ARGUS_CORE_SERVERS_RTMP_CHUNK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define IRS3_RTMP_MAX_CHUNK_STREAMS 64

typedef struct irs3_rtmp_message {
    uint8_t type_id;
    uint32_t timestamp_ms;
    uint32_t message_stream_id;
    size_t payload_len;
    uint8_t *payload;
} irs3_rtmp_message;

typedef struct irs3_rtmp_chunk_stream {
    int initialized;
    uint32_t timestamp;
    uint32_t timestamp_delta;
    uint32_t message_length;
    uint8_t message_type_id;
    uint32_t message_stream_id;
    size_t bytes_read;
    uint8_t *message;
    size_t message_capacity;
} irs3_rtmp_chunk_stream;

typedef struct irs3_rtmp_chunk_reader {
    uint32_t in_chunk_size;
    irs3_rtmp_chunk_stream streams[IRS3_RTMP_MAX_CHUNK_STREAMS];
} irs3_rtmp_chunk_reader;

void irs3_rtmp_chunk_reader_init(irs3_rtmp_chunk_reader *reader);
void irs3_rtmp_chunk_reader_free(irs3_rtmp_chunk_reader *reader);
int irs3_rtmp_read_message(int fd, irs3_rtmp_chunk_reader *reader, irs3_rtmp_message *message);
void irs3_rtmp_message_free(irs3_rtmp_message *message);

#ifdef __cplusplus
}
#endif

#endif
