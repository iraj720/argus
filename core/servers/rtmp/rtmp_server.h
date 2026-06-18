#ifndef ARGUS_CORE_SERVERS_RTMP_SERVER_H
#define ARGUS_CORE_SERVERS_RTMP_SERVER_H

#ifdef __cplusplus
#include "core/runtime/iserver.h"

#include <memory>
#endif

#include "core/servers/rtmp/rtmp_session.h"

#ifdef __cplusplus
std::unique_ptr<irs3::IServer> irs3_create_rtmp_server(const irs3_rtmp_server_config &config);
#endif

#ifdef __cplusplus
extern "C" {
#endif

int irs3_rtmp_server_run(const irs3_rtmp_server_config *config);

#ifdef __cplusplus
}
#endif

#endif
