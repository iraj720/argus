#ifndef ARGUS_CORE_SERVERS_RTMP_SESSION_H
#define ARGUS_CORE_SERVERS_RTMP_SESSION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct irs3_rtmp_server_config {
    char bind_host[128];
    uint16_t port;
    char output_root[1024];
} irs3_rtmp_server_config;

typedef struct irs3_rtmp_session_args {
    int client_fd;
    unsigned long session_id;
    irs3_rtmp_server_config config;
    void *observer_context;
    void (*on_publish)(
        void *observer_context,
        unsigned long session_id,
        int client_fd,
        const char *app,
        const char *stream_name
    );
    void (*on_close)(
        void *observer_context,
        unsigned long session_id,
        int client_fd
    );
    int (*on_media)(
        void *observer_context,
        unsigned long session_id,
        uint8_t type_id,
        uint32_t timestamp_ms,
        const uint8_t *payload,
        size_t payload_len
    );
} irs3_rtmp_session_args;

void *irs3_rtmp_session_thread_main(void *opaque);

#ifdef __cplusplus
}
#endif

#endif
