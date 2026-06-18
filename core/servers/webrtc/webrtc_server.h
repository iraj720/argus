#ifndef ARGUS_CORE_SERVERS_WEBRTC_SERVER_H
#define ARGUS_CORE_SERVERS_WEBRTC_SERVER_H

#ifdef __cplusplus
#include "core/runtime/iserver.h"

#include <memory>
#endif

#include <stdint.h>

typedef struct irs3_webrtc_server_config {
    char bind_host[128];
    uint16_t port;
    char output_root[1024];
} irs3_webrtc_server_config;

#ifdef __cplusplus
std::unique_ptr<irs3::IServer> irs3_create_webrtc_server(const irs3_webrtc_server_config &config);
extern "C" {
#endif

int irs3_webrtc_server_run(const irs3_webrtc_server_config *config);

#ifdef __cplusplus
}
#endif

#endif
