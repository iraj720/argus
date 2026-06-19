#include "core/runtime/runtime.h"

#include "core/sources/subscription_filter.h"

#include <algorithm>
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
    if (!config_.initial_manifest.sources.empty() || !config_.initial_manifest.sinks.empty()) {
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
    std::vector<std::string> sink_ids_to_try;

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
            active_source.route_sink_ids = sink_ids_it->second;
            sink_ids_to_try = sink_ids_it->second;
        }
        state_.active_sources.emplace(descriptor.id, std::move(active_source));
    }

    std::fprintf(
        stderr,
        "argus: source accepted id=%s protocol=%s\n",
        descriptor.id.c_str(),
        protocol_name(descriptor.protocol)
    );

    {
        std::lock_guard<std::mutex> lock(state_.mutex);
        TryStartPendingSinksLocked();
        (void)sink_ids_to_try;
    }
}

void Runtime::CloseActiveState() {
    std::vector<std::unique_ptr<SinkHandle>> sink_handles;
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
            if (desired_sink_it == new_desired.sinks_by_id.end()) {
                route_ids_to_remove.push_back(sink_id);
                continue;
            }
            const RuntimeActiveSinkState &active_sink = state_.active_sinks.at(sink_id);
            if (!SinkSpecsEqual(desired_sink_it->second, active_sink.spec)) {
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

        state_.desired = std::move(new_desired);
        TryStartPendingSinksLocked();
    }

    for (RuntimeActiveRouteState &route_state : routes_to_close) {
        CloseRouteState(&route_state);
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
    for (SourceSubscriptionPtr &subscription : route_state->subscriptions) {
        if (subscription != nullptr) {
            subscription->Close();
        }
    }
}

bool Runtime::AllSinkSourcesActiveLocked(const RuntimeSinkSpec &sink_spec) const {
    for (const RuntimeSinkInputSpec &input : sink_spec.inputs) {
        if (state_.active_sources.find(input.id) == state_.active_sources.end()) {
            return false;
        }
    }
    return true;
}

bool Runtime::StartRouteLocked(const std::string &sink_id, const RuntimeSinkSpec &sink_spec) {
    if (state_.active_routes.find(sink_id) != state_.active_routes.end()) {
        return true;
    }
    if (!AllSinkSourcesActiveLocked(sink_spec)) {
        return false;
    }

    std::vector<RemuxSinkInput> inputs;
    inputs.reserve(sink_spec.inputs.size());
    std::vector<SourceSubscriptionPtr> subscriptions;
    subscriptions.reserve(sink_spec.inputs.size());

    for (const RuntimeSinkInputSpec &input_spec : sink_spec.inputs) {
        auto source_it = state_.active_sources.find(input_spec.id);
        if (source_it == state_.active_sources.end() || source_it->second.source == nullptr) {
            return false;
        }

        SubscriptionFilter filter = MakeSubscriptionFilter(
            input_spec.stream_type,
            input_spec.packet_type,
            input_spec.stream_id
        );
        SourceSubscriptionPtr subscription = source_it->second.source->Subscribe(filter);
        if (subscription == nullptr) {
            return false;
        }

        RemuxSinkInput input;
        input.source = source_it->second.source;
        input.subscription = subscription;
        input.input = input_spec;
        inputs.push_back(std::move(input));
        subscriptions.push_back(subscription);
    }

    auto sink_handle = std::make_unique<SinkHandle>(sink_spec, std::move(inputs));
    if (!sink_handle->Start()) {
        for (SourceSubscriptionPtr &subscription : subscriptions) {
            if (subscription != nullptr) {
                subscription->Close();
            }
        }
        return false;
    }

    RuntimeActiveSinkState active_sink;
    active_sink.spec = sink_spec;
    state_.active_sinks[sink_id] = std::move(active_sink);

    RuntimeActiveRouteState route_state;
    route_state.sink_id = sink_id;
    for (const RuntimeSinkInputSpec &input_spec : sink_spec.inputs) {
        if (std::find(route_state.source_ids.begin(), route_state.source_ids.end(), input_spec.id) ==
            route_state.source_ids.end()) {
            route_state.source_ids.push_back(input_spec.id);
        }
    }
    route_state.subscriptions = std::move(subscriptions);
    route_state.sink_handle = std::move(sink_handle);
    state_.active_routes.emplace(sink_id, std::move(route_state));

    std::fprintf(stderr, "argus: sink started id=%s inputs=%zu\n", sink_id.c_str(), sink_spec.inputs.size());
    return true;
}

void Runtime::TryStartPendingSinksLocked() {
    for (const auto &entry : state_.desired.sinks_by_id) {
        const std::string &sink_id = entry.first;
        const RuntimeSinkSpec &sink_spec = entry.second;
        if (state_.active_routes.find(sink_id) != state_.active_routes.end()) {
            continue;
        }
        (void)StartRouteLocked(sink_id, sink_spec);
    }
}

} // namespace irs3
