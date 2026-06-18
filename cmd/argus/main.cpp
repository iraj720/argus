#include "core/servers/rtmp/rtmp_server.h"
#include "core/compose/compose_type.h"
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
#include <unordered_map>
#include <vector>

namespace {

void usage(const char *prog) {
    std::fprintf(
        stderr,
        "usage: %s [--server rtmp|webrtc|both] [--host 127.0.0.1] [--port 1935] [--rtmp-port 1935] [--webrtc-port 8080] [--live] [--source live/test --sink ./record] [--decoder] [--compose ./frames] [--compose-side-by-side ./scenes] [--compose-infer-prompt \"a minion\" ./infer] [--sink ./record2] [--source live/other --sink ./record3]\n",
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

irs3::RuntimeDecoderSpec make_decoder_for_source(const std::string &source_id, std::size_t index) {
    irs3::RuntimeDecoderSpec decoder;
    decoder.source_id = source_id;
    decoder.decoder_id = source_id + "#decoder-" + std::to_string(index);
    return decoder;
}

bool ensure_decoder_for_compose(
    const std::string &current_source_id,
    std::string *current_decoder_id,
    irs3::RuntimeManifest *manifest
) {
    if (!current_decoder_id->empty()) {
        return true;
    }
    if (current_source_id.empty()) {
        return false;
    }
    irs3::RuntimeDecoderSpec decoder = make_decoder_for_source(current_source_id, manifest->decoders.size());
    *current_decoder_id = decoder.decoder_id;
    manifest->decoders.push_back(std::move(decoder));
    return true;
}

void push_compose(
    irs3::RuntimeManifest *manifest,
    const std::string &current_decoder_id,
    const char *compose_type,
    std::string output_root,
    std::string prompt = {},
    std::string model_root = {}
) {
    irs3::RuntimeComposeSpec compose;
    compose.output_root = std::move(output_root);
    compose.decoder_id = current_decoder_id;
    compose.compose_type = compose_type;
    compose.prompt = std::move(prompt);
    compose.model_root = std::move(model_root);
    compose.compose_id = current_decoder_id + "#compose-" + std::to_string(manifest->composes.size());
    manifest->composes.push_back(std::move(compose));
}

} // namespace

int main(int argc, char **argv) {
    std::array<char, 128> bind_host{};
    irs3::RuntimeManifest manifest;
    std::string_view server = "rtmp";
    std::uint16_t port = 0;
    std::uint16_t rtmp_port = 0;
    std::uint16_t runtime_port = 0;
    std::uint16_t webrtc_port = 0;
    irs3_hls_sink_output_mode pending_sink_mode = IRS3_HLS_SINK_OUTPUT_RECORD;
    bool port_set = false;
    bool rtmp_port_set = false;
    bool runtime_port_set = false;
    bool webrtc_port_set = false;

    copy_cstr(&bind_host, "127.0.0.1");
    std::string current_source_id;
    std::string current_decoder_id;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--server" && i + 1 < argc) {
            server = argv[++i];
        } else if (arg == "--host" && i + 1 < argc) {
            copy_cstr(&bind_host, argv[++i]);
        } else if (arg == "--port" && i + 1 < argc) {
            if (parse_port(argv[++i], &port) != 0) {
                usage(argv[0]);
                return 1;
            }
            port_set = true;
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
        } else if ((arg == "--sink" || arg == "--output-root") && i + 1 < argc) {
            if (current_source_id.empty()) {
                usage(argv[0]);
                return 1;
            }
            irs3::RuntimeSinkSpec sink;
            sink.output_root = argv[++i];
            sink.output_mode = pending_sink_mode;
            sink.source_id = current_source_id;
            sink.sink_id = current_source_id + "#" + std::to_string(manifest.sinks.size());
            manifest.sinks.push_back(std::move(sink));
            pending_sink_mode = IRS3_HLS_SINK_OUTPUT_RECORD;
        } else if (arg == "--decoder") {
            if (current_source_id.empty()) {
                usage(argv[0]);
                return 1;
            }
            irs3::RuntimeDecoderSpec decoder = make_decoder_for_source(current_source_id, manifest.decoders.size());
            current_decoder_id = decoder.decoder_id;
            manifest.decoders.push_back(std::move(decoder));
        } else if (arg == "--compose" && i + 1 < argc) {
            if (current_source_id.empty()) {
                usage(argv[0]);
                return 1;
            }
            if (!ensure_decoder_for_compose(current_source_id, &current_decoder_id, &manifest)) {
                usage(argv[0]);
                return 1;
            }
            push_compose(&manifest, current_decoder_id, irs3::kComposeTypeJpgSnapshot, argv[++i]);
        } else if (arg == "--compose-side-by-side" && i + 1 < argc) {
            if (current_source_id.empty()) {
                usage(argv[0]);
                return 1;
            }
            if (!ensure_decoder_for_compose(current_source_id, &current_decoder_id, &manifest)) {
                usage(argv[0]);
                return 1;
            }
            push_compose(&manifest, current_decoder_id, irs3::kComposeTypeSideBySide, argv[++i]);
        } else if (arg == "--compose-infer-prompt" && i + 2 < argc) {
            if (current_source_id.empty()) {
                usage(argv[0]);
                return 1;
            }
            if (!ensure_decoder_for_compose(current_source_id, &current_decoder_id, &manifest)) {
                usage(argv[0]);
                return 1;
            }
            const char *prompt = argv[++i];
            push_compose(
                &manifest,
                current_decoder_id,
                irs3::kComposeTypeClipPrompt,
                argv[++i],
                prompt
            );
        } else if (arg == "--source" && i + 1 < argc) {
            current_source_id = argv[++i];
            current_decoder_id.clear();
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
        manifest.sources.push_back(irs3::RuntimeSourceSpec{"live/test"});
        irs3::RuntimeSinkSpec sink;
        sink.sink_id = "live/test#0";
        sink.source_id = "live/test";
        sink.output_root = "./out/argus";
        sink.output_mode = IRS3_HLS_SINK_OUTPUT_RECORD;
        manifest.sinks.push_back(std::move(sink));
    } else {
        std::unordered_map<std::string, std::size_t> sink_counts_by_source_id;
        std::unordered_map<std::string, std::size_t> decoder_counts_by_source_id;
        for (const irs3::RuntimeSinkSpec &sink : manifest.sinks) {
            sink_counts_by_source_id[sink.source_id] += 1;
        }
        for (const irs3::RuntimeDecoderSpec &decoder : manifest.decoders) {
            decoder_counts_by_source_id[decoder.source_id] += 1;
        }
        for (const irs3::RuntimeSourceSpec &source : manifest.sources) {
            if (sink_counts_by_source_id[source.source_id] == 0 &&
                decoder_counts_by_source_id[source.source_id] == 0 &&
                manifest.composes.empty()) {
                usage(argv[0]);
                return 1;
            }
        }
    }

    irs3::RuntimeConfig runtime_config{};
    runtime_config.http.bind_host = bind_host.data();
    runtime_config.http.port = runtime_port_set ? runtime_port : 8090;
    runtime_config.http.enabled = true;
    runtime_config.initial_manifest = manifest;

    if (port_set) {
        usage(argv[0]);
        return 1;
    }

    irs3_rtmp_server_config rtmp_config{};
    std::snprintf(rtmp_config.bind_host, sizeof(rtmp_config.bind_host), "%s", bind_host.data());
    rtmp_config.port = rtmp_port_set ? rtmp_port : 1935;
    rtmp_config.output_root[0] = '\0';

    irs3_webrtc_server_config webrtc_config{};
    std::snprintf(webrtc_config.bind_host, sizeof(webrtc_config.bind_host), "%s", bind_host.data());
    webrtc_config.port = webrtc_port_set ? webrtc_port : 8080;
    webrtc_config.output_root[0] = '\0';

    std::vector<irs3::ServerPtr> servers;
    servers.push_back(irs3_create_rtmp_server(rtmp_config));
    servers.push_back(irs3_create_webrtc_server(webrtc_config));
    irs3::Runtime runtime(std::move(runtime_config), std::move(servers));
    return runtime.Start();
}
