#include "core/servers/rtmp/rtmp_server.h"
#include "core/runtime/runtime_manifest.h"
#include "core/runtime/runtime.h"
#include "core/servers/webrtc/webrtc_server.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

void usage(const char *prog) {
    std::fprintf(
        stderr,
        "usage: %s [--server rtmp|webrtc|both] [--host 127.0.0.1] [--rtmp-port 1935] [--webrtc-port 8080] [--runtime-port 8090] [--live] [--voice aac|opus] [--source live/test] [--sink ./record] [--sink-id rec-a]\n",
        prog
    );
}

template <size_t N>
void copy_cstr(std::array<char, N> *dst, const char *src) {
    std::snprintf(dst->data(), dst->size(), "%s", src);
}

int parse_port(const char *value, std::uint16_t *port) {
    char *end = nullptr;
    unsigned long parsed = std::strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed > 65535ul) {
        return -1;
    }
    *port = static_cast<std::uint16_t>(parsed);
    return 0;
}

irs3::RuntimeSinkInputSpec make_default_video_input(const std::string &source_id) {
    irs3::RuntimeSinkInputSpec input;
    input.kind = "source";
    input.id = source_id;
    input.stream_type = "video";
    input.packet_type = "video/h264";
    input.stream_id = "video/main";
    return input;
}

irs3::RuntimeSinkInputSpec make_default_voice_input(const std::string &source_id, const std::string &packet_type) {
    irs3::RuntimeSinkInputSpec input;
    input.kind = "source";
    input.id = source_id;
    input.stream_type = "voice";
    input.packet_type = packet_type;
    input.stream_id = "voice/main";
    return input;
}

irs3::RuntimeSinkSpec make_default_sink(
    const std::string &source_id,
    std::string output_root,
    irs3_hls_sink_output_mode mode,
    const std::string &sink_id,
    const std::string &voice_packet_type
) {
    irs3::RuntimeSinkSpec sink;
    sink.sink_id = sink_id.empty() ? source_id + "#0" : sink_id;
    sink.output_root = std::move(output_root);
    sink.output_mode = mode;
    sink.inputs.push_back(make_default_video_input(source_id));
    sink.inputs.push_back(make_default_voice_input(source_id, voice_packet_type));
    return sink;
}

} // namespace

int main(int argc, char **argv) {
    std::array<char, 128> bind_host{};
    irs3::RuntimeManifest manifest;
    manifest.manifest_version = irs3::kRuntimeManifestVersion;
    std::string_view server = "both";
    std::uint16_t rtmp_port = 0;
    std::uint16_t runtime_port = 0;
    std::uint16_t webrtc_port = 0;
    irs3_hls_sink_output_mode pending_sink_mode = IRS3_HLS_SINK_OUTPUT_RECORD;
    bool rtmp_port_set = false;
    bool runtime_port_set = false;
    bool webrtc_port_set = false;

    std::string pending_voice_packet_type = "voice/aac";
    bool voice_packet_type_set = false;

    copy_cstr(&bind_host, "127.0.0.1");
    std::string current_source_id;
    std::string pending_sink_id;
    std::string pending_output_root;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--server" && i + 1 < argc) {
            server = argv[++i];
            if (!voice_packet_type_set && server == "webrtc") {
                pending_voice_packet_type = "voice/opus";
            }
        } else if (arg == "--host" && i + 1 < argc) {
            copy_cstr(&bind_host, argv[++i]);
        } else if (arg == "--port" && i + 1 < argc) {
            usage(argv[0]);
            return 1;
        } else if (arg == "--rtmp-port" && i + 1 < argc) {
            if (parse_port(argv[++i], &rtmp_port) != 0) {
                usage(argv[0]);
                return 1;
            }
            rtmp_port_set = true;
        } else if (arg == "--runtime-port" && i + 1 < argc) {
            if (parse_port(argv[++i], &runtime_port) != 0) {
                usage(argv[0]);
                return 1;
            }
            runtime_port_set = true;
        } else if (arg == "--webrtc-port" && i + 1 < argc) {
            if (parse_port(argv[++i], &webrtc_port) != 0) {
                usage(argv[0]);
                return 1;
            }
            webrtc_port_set = true;
        } else if (arg == "--live") {
            pending_sink_mode = IRS3_HLS_SINK_OUTPUT_LIVE;
        } else if (arg == "--voice" && i + 1 < argc) {
            const std::string_view voice = argv[++i];
            if (voice == "aac") {
                pending_voice_packet_type = "voice/aac";
            } else if (voice == "opus") {
                pending_voice_packet_type = "voice/opus";
            } else {
                usage(argv[0]);
                return 1;
            }
            voice_packet_type_set = true;
        } else if (arg == "--sink-id" && i + 1 < argc) {
            pending_sink_id = argv[++i];
        } else if ((arg == "--sink" || arg == "--output-root") && i + 1 < argc) {
            if (current_source_id.empty()) {
                usage(argv[0]);
                return 1;
            }
            pending_output_root = argv[++i];
            manifest.sinks.push_back(make_default_sink(
                current_source_id,
                pending_output_root,
                pending_sink_mode,
                pending_sink_id,
                pending_voice_packet_type
            ));
            pending_sink_id.clear();
            pending_output_root.clear();
            pending_sink_mode = IRS3_HLS_SINK_OUTPUT_RECORD;
        } else if (arg == "--source" && i + 1 < argc) {
            current_source_id = argv[++i];
            manifest.sources.push_back(irs3::RuntimeSourceSpec{current_source_id});
        } else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (manifest.sources.empty() && manifest.sinks.empty()) {
        if (!voice_packet_type_set && server == "webrtc") {
            pending_voice_packet_type = "voice/opus";
        }
        manifest.sources.push_back(irs3::RuntimeSourceSpec{"live/test"});
        manifest.sinks.push_back(make_default_sink(
            "live/test",
            "./out/argus",
            IRS3_HLS_SINK_OUTPUT_RECORD,
            "",
            pending_voice_packet_type
        ));
    }

    irs3::RuntimeConfig runtime_config{};
    runtime_config.http.bind_host = bind_host.data();
    runtime_config.http.port = runtime_port_set ? runtime_port : 8090;
    runtime_config.http.enabled = true;
    runtime_config.initial_manifest = manifest;

    irs3_rtmp_server_config rtmp_config{};
    std::snprintf(rtmp_config.bind_host, sizeof(rtmp_config.bind_host), "%s", bind_host.data());
    rtmp_config.port = rtmp_port_set ? rtmp_port : 1935;
    rtmp_config.output_root[0] = '\0';

    irs3_webrtc_server_config webrtc_config{};
    std::snprintf(webrtc_config.bind_host, sizeof(webrtc_config.bind_host), "%s", bind_host.data());
    webrtc_config.port = webrtc_port_set ? webrtc_port : 8080;
    webrtc_config.output_root[0] = '\0';

    std::vector<irs3::ServerPtr> servers;
    if (server == "rtmp" || server == "both") {
        servers.push_back(irs3_create_rtmp_server(rtmp_config));
    }
    if (server == "webrtc" || server == "both") {
        servers.push_back(irs3_create_webrtc_server(webrtc_config));
    }
    if (servers.empty()) {
        usage(argv[0]);
        return 1;
    }

    irs3::Runtime runtime(std::move(runtime_config), std::move(servers));
    return runtime.Start();
}
