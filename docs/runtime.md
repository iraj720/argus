# Runtime Design

`argus` is moving toward a runtime-managed pipeline.

For this phase, `main` should only bootstrap process-level config, create the ingest servers, create the runtime, and run it.

## Goal

The runtime owns:

- process-level ingest servers
- desired pipeline state
- active sources
- active sinks
- source-to-sink subscriptions
- the HTTP control API used to reconcile the pipeline

For now, the pipeline graph is intentionally small:

- `Source`
- `Sink`
- `Sink -> Source` binding

Servers are not graph nodes. They are created from process config and kept outside the manifest.

## Runtime Responsibilities

The runtime is responsible for:

- starting and stopping RTMP and/or WHIP ingest servers
- collecting newly accepted sources from servers
- registering currently active sources by protocol-independent `source_id`
- exposing an HTTP API with one endpoint: `upsert_manifest`
- validating manifests before mutating any runtime state
- reconciling sinks and source subscriptions against the latest manifest
- allowing multiple sinks to subscribe to one source
- stopping and removing obsolete sinks and routes
- rejecting manifests that reference sources not currently active
- keeping shutdown and reconcile operations thread-safe

The runtime is not responsible for:

- RTMP or WHIP protocol handling details
- sink media remux logic
- source packet buffering internals
- making servers aware of sinks
- making sources aware of sink concrete types

## Existing Abstractions To Build On

The first runtime implementation should build on the existing seams:

- `IServer`
- `ISource`
- `ISourceSubscription`
- `BufferedSource`
- `HlsSinkRunner`

This keeps the runtime as an orchestration layer instead of forcing another redesign before integration.

## Process-Level Model

Servers are created by config at process startup.

Examples:

- RTMP only
- WHIP only
- both RTMP and WHIP in the same process

These server instances are passed into the runtime at construction time. They are not created or destroyed by manifest content.

## Graph Model

For now, the graph only contains sources and sinks.

The agreed logical model is:

- `desired_sources[source_id]`
- `desired_sinks[sink_id]`
- `desired_routes[sink_id] = source_id`

This means:

- source identity is `source_id`
- sink identity is `sink_id`
- each sink binds to exactly one source for now
- one source may fan out to many sinks

Routes are modeled implicitly by each sink's target `source_id`.

## Source Identity

Sources use protocol-independent ids such as:

- `live/cam-a`
- `live/cam-b`

Protocol remains metadata on the active source descriptor, but the runtime graph does not use protocol as part of source identity.

This means a manifest route targets the logical stream id, not `rtmp/live/cam-a` or `whip/live/cam-a`.

## Sink Identity

Each sink must have a stable `sink_id`.

Recommended sink fields:

- `sink_id`
- `source_id`
- `output_root`
- `mode`

Where `mode` is one of:

- `record`
- `live`

## Manifest Contract

For now, `upsert_manifest` is a full-replace desired-state update.

That means:

- if a source, sink, or binding exists in the new manifest and already exists at runtime, keep it
- if it is new, create it
- if it is absent from the new manifest, remove it

The runtime must validate the whole manifest before mutating state.

If validation fails, the runtime rejects the request and keeps the previous graph unchanged.

## Manifest Shape

Related docs:

- `docs/scenarios/complex_two_source.md` — multi-source manifest stress case
- `docs/compose.md` — compose graph, `kind: source` inputs

The initial remux-only shape:

```json
{
  "sources": [
    { "source_id": "live/cam-a" },
    { "source_id": "live/cam-b" }
  ],
  "sinks": [
    {
      "sink_id": "record-a",
      "source_id": "live/cam-a",
      "output_root": "./record-a",
      "mode": "record"
    },
    {
      "sink_id": "live-b",
      "source_id": "live/cam-b",
      "output_root": "./live-b",
      "mode": "live"
    }
  ]
}
```

Notes:

- `sources` declares the set of logical sources expected by the pipeline
- `sinks` both declare sink nodes and declare the route through `source_id`
- remux-branch sinks bind to `source_id`; encode-branch sinks bind to `encoder_id` **(target)**

### Decoders **(target)**

One decoder per **`stream_type` per source** that requires decode:

```json
{
  "decoder_id": "voice-dec-a",
  "source_id": "live/studio",
  "stream_type": "voice",
  "packet_type": "voice/aac"
}
```

Multiple voice **`stream_id`s** on one source (main + instruction mic) share one
voice decoder; composes filter by `stream_id` on inputs.

### Sink kinds **(target)**

| Branch | Manifest binding | Pulls / muxes |
|--------|------------------|---------------|
| **Remux** | `source_id` + **`inputs[]`** | Encoded packets from source subscription; **muxes** selected streams into **one** output (HLS/file) |
| **Encode output** | `encoder_id` | Encoded packets from encoder subscription (already encoded/muxed by encoder) |

Remux sinks do **not** use one sink per `packet_type`. One sink declares **two or
more** (or fewer) encoded inputs and remuxes them:

```json
{
  "sink_id": "rec-a",
  "source_id": "live/studio",
  "output_root": "./record/studio",
  "mode": "record",
  "inputs": [
    { "packet_type": "video/h264", "stream_id": "video/main" },
    { "packet_type": "voice/aac", "stream_id": "voice/main" }
  ]
}
```

Each `inputs[]` entry filters which packets from the source timeline the sink
includes in the mux. Optional `stream_id` narrows to one track.

Encode-branch sinks bind to **`encoder_id`** only — no `inputs[]` on the sink;
the encoder already produced the encoded program.

A single process may run **both** branches for the same source (one remux record
+ decode/compose/encode path). See `docs/scenarios/complex_two_source.md`.

### Compose and encode branch **(target)**

Compose nodes, encoders, and encode-branch sinks use **inputs-only** wiring.
See `docs/compose.md` for the full model.

Summary:

- `composes[].inputs[]` — `{ "kind": "source"|"decoder"|"compose", "id": "...", ...filters }`
- `encoders[].inputs[]` — references compose node(s) the encoder pulls from
- encode-branch sinks use **`encoder_id`** (not `source_id`)
- **no node declares downstream**
- compose graph must be a DAG; fan-out is allowed (many readers reference the same upstream id)
- **current code** may still accept legacy `decoder_id` on composes until migration

Full two-source example manifest: `docs/scenarios/complex_two_source.md`.

## Validation Rules

Before reconciliation, the runtime should validate:

- duplicate `source_id`, `sink_id`, `decoder_id`, `compose_id`, and `encoder_id` values are rejected
- remux sinks: `source_id`, non-empty `output_root`, non-empty **`inputs[]`**
  (each entry filters `packet_type` / optional `stream_id`) **(target)**
- encode sinks: each references `encoder_id` and non-empty `output_root` **(target)**
- each sink has a supported `mode`
- each `source_id` referenced by a sink or decoder is active in runtime state (strict upsert)

Compose / encoder validation **(target)** — see `docs/compose.md`:

- each compose has non-empty `inputs`
- each `inputs[].kind` is `source`, `decoder`, or `compose`; `id` resolves in manifest
- decoders declare `source_id`, `stream_type`, and `packet_type`
- no cycles in compose references
- per-`compose_type` params validated independently

Important:

- if a sink references a source that is not currently publishing and active, the whole `upsert_manifest` request is rejected
- the runtime should not partially apply a manifest

## Desired State vs Active State

Desired state and active state should be kept separate.

Recommended desired-state maps:

- `desired_sources[source_id]`
- `desired_sinks[sink_id]`
- `desired_routes[sink_id] = source_id`

Recommended active-state maps:

- `active_sources[source_id] -> SourcePtr`
- `active_sinks[sink_id] -> SinkHandle`
- `active_routes[sink_id] -> RouteHandle`

Where:

- `SinkHandle` wraps runtime-owned sink lifecycle
- `RouteHandle` owns the subscription between one sink and one source

## Source Semantics

The source owns:

- the canonical packet buffer
- source metadata
- source readiness
- subscriber registry

Each sink gets its own subscription and its own cursor into the source.

The current direction for source fanout is:

- one source buffer
- one subscription object per downstream consumer
- independent read cursor per subscription
- if a subscriber falls behind stale data, it is resumed at the next available GOP boundary

This keeps the source independent of concrete sink types and supports many sinks on top of one publisher session.

## Sink Semantics

Sinks should not be managed by servers.

Sinks should be managed by the runtime.

For each active route:

- runtime creates a subscription from the source
- runtime creates a sink runner for that sink
- the sink runner consumes packets through the subscription

This means:

- servers do not know about sinks
- sources know subscribers, not sinks
- runtime owns the connection graph

## Reconcile Behavior

On each accepted `upsert_manifest`, reconciliation should behave as follows:

1. Parse and validate the manifest.
2. If any validation fails, reject the request and keep the current graph unchanged.
3. Create any new sink nodes.
4. Create any new source-to-sink subscriptions.
5. Keep any existing sink and route whose config is unchanged.
6. Remove obsolete routes.
7. Stop and remove obsolete sinks.
8. Remove obsolete desired-state entries.

For now, this is a cold full-replace reconcile. Warm swap behavior is explicitly deferred.

## Multiple Sinks Per Source

Multiple sinks reading from one source is a required runtime behavior.

The intended implementation is:

- one active source per `source_id`
- many sink routes may point to the same `source_id`
- each route receives its own `ISourceSubscription`
- each sink runner reads independently

This allows:

- one record sink and one live sink on the same source
- many output roots for one inbound publisher
- sink isolation without duplicating server or source ownership

## Runtime Public API

The runtime API should stay narrow.

Recommended shape:

```cpp
class Runtime {
public:
    Runtime(RuntimeConfig config, std::vector<ServerPtr> servers);

    int Start();
    void Close();
    ApplyManifestResult ApplyManifest(const RuntimeManifest &manifest);
};
```

The HTTP layer should be a thin adapter over `ApplyManifest`.

## HTTP API

For now, the runtime exposes one API:

- `POST /upsert_manifest`

Expected behavior:

- parse request body into manifest DTOs
- validate the full requested graph
- apply atomically if valid
- reject with a structured error if invalid

Because manifests are full-replace for now, omitted nodes are treated as removals.

## Main Program Shape

`main` should become a thin bootstrap layer.

It should do only the following:

- parse process-level config
- create RTMP and/or WHIP server instances
- create the runtime
- start the runtime
- wait for shutdown

It should not own the connection graph directly.

## Runtime Internal Components

The runtime implementation should live under `core/runtime/`.

Recommended files:

- `core/runtime/runtime.h`
- `core/runtime/runtime.cpp`
- `core/runtime/runtime_manifest.h`
- `core/runtime/runtime_manifest.cpp`
- `core/runtime/runtime_http.h`
- `core/runtime/runtime_http.cpp`
- `core/runtime/runtime_state.h`
- `core/runtime/sink_handle.h`
- `core/runtime/sink_handle.cpp`

This keeps orchestration code out of `main.cpp` and centralizes lifecycle control.

## Close Order

`Runtime::Close()` should shut down from the outside in. Full thread ownership,
startup sequence, buffer fill order, and manifest update ordering are documented
in `docs/execution_model.md`.

Recommended order:

1. stop accepting new HTTP requests
2. freeze reconciliation and mark runtime as shutting down
3. close active routes and subscriptions
4. close active sinks and join sink worker threads
5. close ingest servers
6. let sources drain and close as connections terminate
7. join source-watcher and server threads
8. clear runtime state maps

The key reason for this order is:

- sinks must stop before sources and servers disappear
- servers must stop before new sources can appear during shutdown
- HTTP must stop first so shutdown does not race with a new manifest

Short form:

- `http -> reconcile -> routes -> sinks -> servers -> sources/watchers -> state`

## Assumptions

- `upsert_manifest` is full-replace for this phase
- warm swap and zero-drop graph transitions are deferred
- a sink may only bind to one source in this phase
- a source may feed many sinks
- sources referenced by manifest must already be active or the request is rejected
- servers are always process-level config and are not manifest-managed
- the first runtime layer will continue to use the existing HLS sink runner and current source abstractions
- target pipeline threading and pull models are specified in `docs/pipeline.md`
- compose nodes are extended pull-based; wiring is inputs-only (`docs/compose.md`)
- on the encode branch, sinks read encoded packets from encoder subscriptions; encoder owns encode/remux
- decoder and compose are extended pull-based (no worker threads); encoder has a runner thread

## Non-Goals For This Phase

The following remain deferred or partial:

- full compose DAG with `inputs[]` in HTTP manifest (legacy `decoder_id` in current code)
- encoder integration on the encode branch
- warm replacement of active routes
- partial manifest merge semantics
- source creation through the manifest itself

Implemented today (outside the original first runtime step):

- decoders and compose nodes (legacy push wiring)
- CLI compose flags

Still out of scope:

- transcoding policy beyond initial encode branch
- protocol-specific graph ids
- sink-to-sink or source-to-source graph edges
