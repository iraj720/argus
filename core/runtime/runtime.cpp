#include "core/runtime/runtime.h"

#include <cstdio>
#include <unordered_set>
#include <utility>

namespace irs3 {

namespace {

const char *protocol_name(SourceProtocol protocol) {
    switch (protocol) {
    case SourceProtocol::kRTMP:
        return "rtmp";
    case SourceProtocol::kWHIP:
        return "whip";
    }
    return "unknown";
}

} // namespace

Runtime::Runtime(RuntimeConfig config, std::vector<ServerPtr> servers)
    : config_(std::move(config)),
      servers_(std::move(servers)) {
    server_ptrs_.reserve(servers_.size());
    for (const auto &server : servers_) {
        server_ptrs_.push_back(server.get());
    }
    http_server_ = std::make_unique<RuntimeHttpServer>(
        config_.http,
        [this](const RuntimeManifest &manifest, std::string *error) {
            ApplyManifestResult result = ApplyManifest(manifest);
            if (!result.ok && error != nullptr) {
                *error = result.error;
            }
            return result.ok;
        }
    );
}

Runtime::~Runtime() {
    Close();
}

int Runtime::Start() {
    if (server_ptrs_.empty()) {
        return 1;
    }
    if (!config_.initial_manifest.sources.empty() || !config_.initial_manifest.sinks.empty() ||
        !config_.initial_manifest.decoders.empty() || !config_.initial_manifest.composes.empty()) {
        ApplyManifestResult bootstrap_result =
            ApplyManifestWithMode(config_.initial_manifest, RuntimeManifestApplyMode::kBootstrap);
        if (!bootstrap_result.ok) {
            std::fprintf(
                stderr,
                "argus: failed to apply bootstrap manifest: %s\n",
                bootstrap_result.error.c_str()
            );
            return 1;
        }
    }
    if (http_server_ != nullptr && !http_server_->Start()) {
        return 1;
    }

    StartServerThreads();

    while (true) {
        SourcePtr source = source_queue_.Pop();
        if (source == nullptr) {
            break;
        }
        HandleSource(std::move(source));
    }

    JoinThreads();
    CloseActiveState();
    return server_result_.load();
}

void Runtime::Close() {
    bool expected = false;
    if (!closed_.compare_exchange_strong(expected, true)) {
        return;
    }

    if (http_server_ != nullptr) {
        http_server_->Close();
    }
    source_queue_.Close();
    for (IServer *server : server_ptrs_) {
        if (server != nullptr) {
            server->Close();
        }
    }
    CloseActiveState();
}

void Runtime::StartServerThreads() {
    remaining_watchers_.store(static_cast<int>(server_ptrs_.size()));

    for (IServer *server : server_ptrs_) {
        server_threads_.emplace_back([this, server]() {
            int result = server->Start();
            if (result != 0) {
                int expected = 0;
                if (server_result_.compare_exchange_strong(expected, result)) {
                    for (IServer *other : server_ptrs_) {
                        if (other != server) {
                            other->Close();
                        }
                    }
                    source_queue_.Close();
                }
            }
        });

        watcher_threads_.emplace_back([this, server]() {
            while (true) {
                SourcePtr source = server->NextSource();
                if (source == nullptr) {
                    break;
                }
                source_queue_.Push(std::move(source));
            }
            if (remaining_watchers_.fetch_sub(1) == 1) {
                source_queue_.Close();
            }
        });
    }
}

void Runtime::JoinThreads() {
    for (std::thread &watcher_thread : watcher_threads_) {
        if (watcher_thread.joinable()) {
            watcher_thread.join();
        }
    }
    for (std::thread &server_thread : server_threads_) {
        if (server_thread.joinable()) {
            server_thread.join();
        }
    }
}

void Runtime::HandleSource(SourcePtr source) {
    if (source == nullptr) {
        return;
    }

    const SourceDescriptor &descriptor = source->Descriptor();
    std::vector<RuntimeSinkSpec> sink_specs_to_start;
    std::vector<RuntimeDecoderSpec> decoder_specs_to_start;
    std::vector<RuntimeComposeSpec> compose_specs_to_start;

    {
        std::lock_guard<std::mutex> lock(state_.mutex);
        if (state_.desired.sources_by_id.find(descriptor.id) == state_.desired.sources_by_id.end()) {
            std::fprintf(
                stderr,
                "argus: source rejected id=%s protocol=%s\n",
                descriptor.id.c_str(),
                protocol_name(descriptor.protocol)
            );
            source->Close();
            return;
        }

        if (state_.active_sources.find(descriptor.id) != state_.active_sources.end()) {
            std::fprintf(
                stderr,
                "argus: source rejected duplicate id=%s protocol=%s\n",
                descriptor.id.c_str(),
                protocol_name(descriptor.protocol)
            );
            source->Close();
            return;
        }

        RuntimeActiveSourceState active_source;
        active_source.source = source;
        active_source.descriptor = descriptor;
        auto sink_ids_it = state_.desired.sink_ids_by_source_id.find(descriptor.id);
        if (sink_ids_it != state_.desired.sink_ids_by_source_id.end()) {
            for (const std::string &sink_id : sink_ids_it->second) {
                active_source.route_sink_ids.push_back(sink_id);
                auto sink_it = state_.desired.sinks_by_id.find(sink_id);
                if (sink_it != state_.desired.sinks_by_id.end()) {
                    sink_specs_to_start.push_back(sink_it->second);
                }
            }
        }
        auto decoder_ids_it = state_.desired.decoder_ids_by_source_id.find(descriptor.id);
        if (decoder_ids_it != state_.desired.decoder_ids_by_source_id.end()) {
            for (const std::string &decoder_id : decoder_ids_it->second) {
                active_source.route_decoder_ids.push_back(decoder_id);
                auto decoder_it = state_.desired.decoders_by_id.find(decoder_id);
                if (decoder_it != state_.desired.decoders_by_id.end()) {
                    decoder_specs_to_start.push_back(decoder_it->second);
                }
                auto compose_ids_it = state_.desired.compose_ids_by_decoder_id.find(decoder_id);
                if (compose_ids_it != state_.desired.compose_ids_by_decoder_id.end()) {
                    for (const std::string &compose_id : compose_ids_it->second) {
                        auto compose_it = state_.desired.composes_by_id.find(compose_id);
                        if (compose_it != state_.desired.composes_by_id.end()) {
                            compose_specs_to_start.push_back(compose_it->second);
                        }
                    }
                }
            }
        }
        state_.active_sources.emplace(descriptor.id, std::move(active_source));
    }

    std::fprintf(
        stderr,
        "argus: source accepted id=%s protocol=%s\n",
        descriptor.id.c_str(),
        protocol_name(descriptor.protocol)
    );

    std::lock_guard<std::mutex> lock(state_.mutex);
    RuntimeActiveSourceState *active_source = nullptr;
    auto source_it = state_.active_sources.find(descriptor.id);
    if (source_it != state_.active_sources.end()) {
        active_source = &source_it->second;
    }
    for (const RuntimeSinkSpec &sink_spec : sink_specs_to_start) {
        (void)StartRouteLocked(sink_spec.sink_id, sink_spec, active_source);
    }
    for (const RuntimeDecoderSpec &decoder_spec : decoder_specs_to_start) {
        (void)StartDecoderRouteLocked(decoder_spec.decoder_id, decoder_spec, active_source);
    }
    for (const RuntimeComposeSpec &compose_spec : compose_specs_to_start) {
        (void)StartComposeRouteLocked(compose_spec.compose_id, compose_spec);
    }
}

void Runtime::CloseActiveState() {
    std::vector<std::unique_ptr<SinkHandle>> sink_handles;
    std::vector<std::unique_ptr<ComposeHandle>> compose_handles;
    std::vector<std::unique_ptr<DecoderHandle>> decoder_handles;
    std::vector<SourcePtr> sources;

    {
        std::lock_guard<std::mutex> lock(state_.mutex);
        sink_handles.reserve(state_.active_routes.size());
        for (auto &entry : state_.active_routes) {
            if (entry.second.sink_handle != nullptr) {
                sink_handles.push_back(std::move(entry.second.sink_handle));
            }
        }
        state_.active_routes.clear();
        state_.active_sinks.clear();

        compose_handles.reserve(state_.active_compose_routes.size());
        for (auto &entry : state_.active_compose_routes) {
            if (entry.second.compose_handle != nullptr) {
                compose_handles.push_back(std::move(entry.second.compose_handle));
            }
        }
        state_.active_compose_routes.clear();
        state_.active_composes.clear();

        decoder_handles.reserve(state_.active_decoder_routes.size());
        for (auto &entry : state_.active_decoder_routes) {
            if (entry.second.decoder_handle != nullptr) {
                decoder_handles.push_back(std::move(entry.second.decoder_handle));
            }
        }
        state_.active_decoder_routes.clear();
        state_.active_decoders.clear();

        sources.reserve(state_.active_sources.size());
        for (auto &entry : state_.active_sources) {
            if (entry.second.source != nullptr) {
                sources.push_back(std::move(entry.second.source));
            }
        }
        state_.active_sources.clear();
    }

    for (const auto &sink_handle : sink_handles) {
        if (sink_handle != nullptr) {
            sink_handle->Close();
        }
    }
    for (const auto &compose_handle : compose_handles) {
        if (compose_handle != nullptr) {
            compose_handle->Close();
        }
    }
    for (const auto &decoder_handle : decoder_handles) {
        if (decoder_handle != nullptr) {
            decoder_handle->Close();
        }
    }
    for (const auto &source : sources) {
        if (source != nullptr) {
            source->Close();
        }
    }
}

ApplyManifestResult Runtime::ApplyManifest(const RuntimeManifest &manifest) {
    return ApplyManifestWithMode(manifest, RuntimeManifestApplyMode::kStrictUpsert);
}

ApplyManifestResult Runtime::ApplyManifestWithMode(const RuntimeManifest &manifest, RuntimeManifestApplyMode mode) {
    std::vector<RuntimeActiveRouteState> routes_to_close;
    std::vector<RuntimeActiveComposeRouteState> compose_routes_to_close;
    std::vector<RuntimeActiveDecoderRouteState> decoder_routes_to_close;
    RuntimeDesiredState new_desired = BuildRuntimeDesiredState(manifest);

    {
        std::lock_guard<std::mutex> lock(state_.mutex);

        std::unordered_set<std::string> active_source_ids;
        for (const auto &entry : state_.active_sources) {
            active_source_ids.insert(entry.first);
        }

        RuntimeManifestValidationResult validation =
            ValidateRuntimeManifest(
                manifest,
                active_source_ids,
                mode == RuntimeManifestApplyMode::kStrictUpsert
            );
        if (!validation.ok) {
            return ApplyManifestResult{false, validation.error};
        }

        std::vector<std::string> route_ids_to_remove;
        for (const auto &entry : state_.active_routes) {
            const std::string &sink_id = entry.first;
            auto desired_sink_it = new_desired.sinks_by_id.find(sink_id);
            auto desired_route_it = new_desired.routes_by_sink_id.find(sink_id);
            if (desired_sink_it == new_desired.sinks_by_id.end() ||
                desired_route_it == new_desired.routes_by_sink_id.end() ||
                desired_route_it->second != entry.second.source_id ||
                desired_sink_it->second.output_root != state_.active_sinks[sink_id].spec.output_root ||
                desired_sink_it->second.output_mode != state_.active_sinks[sink_id].spec.output_mode) {
                route_ids_to_remove.push_back(sink_id);
            }
        }

        for (const std::string &sink_id : route_ids_to_remove) {
            auto route_it = state_.active_routes.find(sink_id);
            if (route_it == state_.active_routes.end()) {
                continue;
            }
            routes_to_close.push_back(std::move(route_it->second));
            state_.active_routes.erase(route_it);
            state_.active_sinks.erase(sink_id);
        }

        std::vector<std::string> compose_route_ids_to_remove;
        for (const auto &entry : state_.active_compose_routes) {
            const std::string &compose_id = entry.first;
            auto desired_compose_it = new_desired.composes_by_id.find(compose_id);
            auto desired_route_it = new_desired.routes_by_compose_id.find(compose_id);
            if (desired_compose_it == new_desired.composes_by_id.end() ||
                desired_route_it == new_desired.routes_by_compose_id.end() ||
                desired_route_it->second != entry.second.decoder_id ||
                desired_compose_it->second.output_root != state_.active_composes[compose_id].spec.output_root ||
                desired_compose_it->second.snapshot_interval != state_.active_composes[compose_id].spec.snapshot_interval ||
                desired_compose_it->second.compose_type != state_.active_composes[compose_id].spec.compose_type ||
                desired_compose_it->second.prompt != state_.active_composes[compose_id].spec.prompt ||
                desired_compose_it->second.model_root != state_.active_composes[compose_id].spec.model_root) {
                compose_route_ids_to_remove.push_back(compose_id);
            }
        }

        for (const std::string &compose_id : compose_route_ids_to_remove) {
            auto route_it = state_.active_compose_routes.find(compose_id);
            if (route_it == state_.active_compose_routes.end()) {
                continue;
            }
            compose_routes_to_close.push_back(std::move(route_it->second));
            state_.active_compose_routes.erase(route_it);
            state_.active_composes.erase(compose_id);
        }

        std::vector<std::string> decoder_route_ids_to_remove;
        for (const auto &entry : state_.active_decoder_routes) {
            const std::string &decoder_id = entry.first;
            auto desired_decoder_it = new_desired.decoders_by_id.find(decoder_id);
            auto desired_route_it = new_desired.routes_by_decoder_id.find(decoder_id);
            if (desired_decoder_it == new_desired.decoders_by_id.end() ||
                desired_route_it == new_desired.routes_by_decoder_id.end() ||
                desired_route_it->second != entry.second.source_id) {
                CloseComposeRoutesForDecoderLocked(decoder_id);
                decoder_route_ids_to_remove.push_back(decoder_id);
            }
        }

        for (const std::string &decoder_id : decoder_route_ids_to_remove) {
            auto route_it = state_.active_decoder_routes.find(decoder_id);
            if (route_it == state_.active_decoder_routes.end()) {
                continue;
            }
            decoder_routes_to_close.push_back(std::move(route_it->second));
            state_.active_decoder_routes.erase(route_it);
            state_.active_decoders.erase(decoder_id);
        }

        state_.desired = std::move(new_desired);

        for (const auto &entry : state_.active_sources) {
            const std::string &source_id = entry.first;
            auto desired_sink_ids_it = state_.desired.sink_ids_by_source_id.find(source_id);
            if (desired_sink_ids_it == state_.desired.sink_ids_by_source_id.end()) {
                continue;
            }
            for (const std::string &sink_id : desired_sink_ids_it->second) {
                if (state_.active_routes.find(sink_id) != state_.active_routes.end()) {
                    continue;
                }
                auto sink_it = state_.desired.sinks_by_id.find(sink_id);
                if (sink_it == state_.desired.sinks_by_id.end()) {
                    continue;
                }
                RuntimeActiveSourceState *active_source = nullptr;
                auto active_source_it = state_.active_sources.find(source_id);
                if (active_source_it != state_.active_sources.end()) {
                    active_source = &active_source_it->second;
                }
                (void)StartRouteLocked(sink_id, sink_it->second, active_source);
            }
        }

        for (const auto &entry : state_.active_sources) {
            const std::string &source_id = entry.first;
            auto desired_decoder_ids_it = state_.desired.decoder_ids_by_source_id.find(source_id);
            if (desired_decoder_ids_it == state_.desired.decoder_ids_by_source_id.end()) {
                continue;
            }
            for (const std::string &decoder_id : desired_decoder_ids_it->second) {
                if (state_.active_decoder_routes.find(decoder_id) != state_.active_decoder_routes.end()) {
                    continue;
                }
                auto decoder_it = state_.desired.decoders_by_id.find(decoder_id);
                if (decoder_it == state_.desired.decoders_by_id.end()) {
                    continue;
                }
                RuntimeActiveSourceState *active_source = nullptr;
                auto active_source_it = state_.active_sources.find(source_id);
                if (active_source_it != state_.active_sources.end()) {
                    active_source = &active_source_it->second;
                }
                (void)StartDecoderRouteLocked(decoder_id, decoder_it->second, active_source);
            }
        }

        for (const auto &entry : state_.active_decoder_routes) {
            const std::string &decoder_id = entry.first;
            auto desired_compose_ids_it = state_.desired.compose_ids_by_decoder_id.find(decoder_id);
            if (desired_compose_ids_it == state_.desired.compose_ids_by_decoder_id.end()) {
                continue;
            }
            for (const std::string &compose_id : desired_compose_ids_it->second) {
                if (state_.active_compose_routes.find(compose_id) != state_.active_compose_routes.end()) {
                    continue;
                }
                auto compose_it = state_.desired.composes_by_id.find(compose_id);
                if (compose_it == state_.desired.composes_by_id.end()) {
                    continue;
                }
                (void)StartComposeRouteLocked(compose_id, compose_it->second);
            }
        }
    }

    for (RuntimeActiveRouteState &route_state : routes_to_close) {
        CloseRouteState(&route_state);
    }
    for (RuntimeActiveComposeRouteState &route_state : compose_routes_to_close) {
        CloseComposeRouteState(&route_state);
    }
    for (RuntimeActiveDecoderRouteState &route_state : decoder_routes_to_close) {
        CloseDecoderRouteState(&route_state);
    }

    return ApplyManifestResult{true, ""};
}

void Runtime::CloseRouteState(RuntimeActiveRouteState *route_state) {
    if (route_state == nullptr) {
        return;
    }
    if (route_state->sink_handle != nullptr) {
        route_state->sink_handle->Close();
    }
    if (route_state->subscription != nullptr) {
        route_state->subscription->Close();
    }
}

bool Runtime::StartRouteLocked(
    const std::string &sink_id,
    const RuntimeSinkSpec &sink_spec,
    RuntimeActiveSourceState *active_source
) {
    if (active_source == nullptr || active_source->source == nullptr) {
        return false;
    }
    if (state_.active_routes.find(sink_id) != state_.active_routes.end()) {
        return true;
    }

    SourceSubscriptionPtr subscription = active_source->source->Subscribe();
    auto sink_handle = std::make_unique<SinkHandle>(active_source->source, subscription, sink_spec);
    if (!sink_handle->Start()) {
        return false;
    }

    RuntimeActiveSinkState active_sink;
    active_sink.spec = sink_spec;
    state_.active_sinks[sink_id] = std::move(active_sink);

    RuntimeActiveRouteState route_state;
    route_state.sink_id = sink_id;
    route_state.source_id = active_source->descriptor.id;
    route_state.subscription = subscription;
    route_state.sink_handle = std::move(sink_handle);
    state_.active_routes.emplace(sink_id, std::move(route_state));
    return true;
}

void Runtime::CloseDecoderRouteState(RuntimeActiveDecoderRouteState *route_state) {
    if (route_state == nullptr) {
        return;
    }
    if (route_state->decoder_handle != nullptr) {
        route_state->decoder_handle->Close();
    }
    if (route_state->subscription != nullptr) {
        route_state->subscription->Close();
    }
}

void Runtime::CloseComposeRouteState(RuntimeActiveComposeRouteState *route_state) {
    if (route_state == nullptr) {
        return;
    }
    if (route_state->compose_handle != nullptr) {
        route_state->compose_handle->Close();
    }
}

void Runtime::CloseComposeRoutesForDecoderLocked(const std::string &decoder_id) {
    std::vector<std::string> compose_ids_to_remove;
    for (const auto &entry : state_.active_compose_routes) {
        if (entry.second.decoder_id == decoder_id) {
            compose_ids_to_remove.push_back(entry.first);
        }
    }
    for (const std::string &compose_id : compose_ids_to_remove) {
        auto route_it = state_.active_compose_routes.find(compose_id);
        if (route_it == state_.active_compose_routes.end()) {
            continue;
        }
        if (route_it->second.compose_handle != nullptr) {
            route_it->second.compose_handle->Close();
        }
        state_.active_compose_routes.erase(route_it);
        state_.active_composes.erase(compose_id);
    }
}

bool Runtime::StartDecoderRouteLocked(
    const std::string &decoder_id,
    const RuntimeDecoderSpec &decoder_spec,
    RuntimeActiveSourceState *active_source
) {
    if (active_source == nullptr || active_source->source == nullptr) {
        return false;
    }
    if (state_.active_decoder_routes.find(decoder_id) != state_.active_decoder_routes.end()) {
        return true;
    }

    SourceSubscriptionPtr subscription = active_source->source->Subscribe();
    auto decoder_handle = std::make_unique<DecoderHandle>(active_source->source, subscription, decoder_spec);
    if (!decoder_handle->Start()) {
        return false;
    }

    RuntimeActiveDecoderState active_decoder;
    active_decoder.spec = decoder_spec;
    state_.active_decoders[decoder_id] = std::move(active_decoder);

    RuntimeActiveDecoderRouteState route_state;
    route_state.decoder_id = decoder_id;
    route_state.source_id = active_source->descriptor.id;
    route_state.subscription = subscription;
    route_state.decoder_handle = std::move(decoder_handle);
    state_.active_decoder_routes.emplace(decoder_id, std::move(route_state));
    return true;
}

bool Runtime::StartComposeRouteLocked(const std::string &compose_id, const RuntimeComposeSpec &compose_spec) {
    if (state_.active_compose_routes.find(compose_id) != state_.active_compose_routes.end()) {
        return true;
    }

    auto decoder_route_it = state_.active_decoder_routes.find(compose_spec.decoder_id);
    if (decoder_route_it == state_.active_decoder_routes.end() ||
        decoder_route_it->second.decoder_handle == nullptr) {
        return false;
    }

    auto compose_handle = std::make_unique<ComposeHandle>(
        decoder_route_it->second.decoder_handle.get(),
        compose_spec
    );
    if (!compose_handle->Start()) {
        return false;
    }

    RuntimeActiveComposeState active_compose;
    active_compose.spec = compose_spec;
    state_.active_composes[compose_id] = std::move(active_compose);

    RuntimeActiveComposeRouteState route_state;
    route_state.compose_id = compose_id;
    route_state.decoder_id = compose_spec.decoder_id;
    route_state.compose_handle = std::move(compose_handle);
    state_.active_compose_routes.emplace(compose_id, std::move(route_state));
    return true;
}

} // namespace irs3
