# Execution Model

This note describes how an `argus` process starts pipelines, which threads
exist, what each thread owns, how buffers fill, and the order for starting,
updating, and closing the runtime.

Related docs:

- `docs/scenarios/complex_two_source.md` — multi-source startup/shutdown case
- `docs/compose.md` — compose graph, inputs-only wiring
- `docs/pipeline.md` — pull models, extended pull-based nodes, encoder branch
- `docs/source.md` — source rings, timeline, subscriptions
- `docs/runtime.md` — manifest, reconcile, HTTP control
- `docs/redesign.md` — component roles and ownership

## Scope

This document covers **process and runtime orchestration**. It describes both:

- **Current code** — what `Runtime::Start()`, `ApplyManifest()`, and `Close()`
  do today (`core/runtime/runtime.cpp`).
- **Target model** — extended pull-based decoder/compose, encoder branch (see
  `docs/pipeline.md`). Sections label **(target)** where behavior differs from
  today's implementation.

## Thread Map

### Process-level threads

| Thread | Created by | Owns / runs |
|--------|------------|-------------|
| **Main** | `main()` | Parses CLI, builds `Runtime`, calls `Runtime::Start()`, blocks until shutdown |
| **Runtime HTTP** | `RuntimeHttpServer::Start()` | `POST /upsert_manifest`, applies manifest updates |
| **Server runner** (one per ingest server) | `Runtime::StartServerThreads()` | `IServer::Start()` — accept loop (RTMP/WHIP) |
| **Source watcher** (one per ingest server) | `Runtime::StartServerThreads()` | `IServer::NextSource()` → pushes `SourcePtr` to `SourceQueue` |
| **Runtime source loop** | `Runtime::Start()` on main thread | `source_queue_.Pop()` → `HandleSource()` |
| **Ingest session** (one per publisher connection) | Server on accept | Protocol read loop; `Publish()` into `BufferedSource` |
| **Sink runner** (one per active sink route) | `SinkHandle::Start()` | Pull loop → HLS write (**current**: from source subscription) |
| **Encoder runner** (one per encoder) | `Encoder::Start()` **(target)** | Pull raw from compose node(s) in encoder `inputs`, encode, fill encoded queue |
| **Decoder runner** | `DecoderHandle::Start()` **(current legacy)** | Background decode thread; **removed in target** |

### What has no thread

| Component | Storage / behavior |
|-----------|-------------------|
| `Source` | Rings + shared timeline; written by ingest session thread only |
| `Decoder` **(target)** | Packet-count buffer; fills on `ReadPacket` |
| `Compose` **(target)** | Packet-count buffer; fills on `ReadPacket`; output via reader subscriptions |
| Subscriptions | Cursor + filter only; never mutate shared buffers |

**N streams in one source does not mean N threads.** One ingest session thread
publishes every `stream_id` into the same source.

## Ownership Summary

```text
Runtime
├── servers[]                    (process config, not manifest-managed)
├── source_queue_                (accepted sources waiting for HandleSource)
├── HTTP server                  (manifest apply entry point)
├── state_.desired               (last applied manifest graph)
└── state_.active_*              (live sources, routes, handles)

Per active source
├── SourcePtr                    (rings + timeline)
├── source subscription(s)       (per sink route and per decoder route today)
├── SinkHandle → HlsSinkRunner   (sink thread)
├── DecoderHandle                (target: no thread; current: DecoderRunner thread)
└── ComposeHandle                (target: no thread; current: callback on decoder thread)

Per encoder (target)
├── inputs[] resolved to compose/decoder pull endpoints
├── encoded output queue
└── encoder subscription(s)      (one per sink on encode branch)
```

Compose nodes are wired from each node's **`inputs`** only — no upstream
downstream fields. See `docs/compose.md`.

## Process Bootstrap

`main` is a thin bootstrap (`cmd/argus/main.cpp`):

```text
1. Parse CLI → RuntimeManifest (sources, sinks, decoders, composes)
2. Build RuntimeConfig (HTTP bind, initial_manifest)
3. Create Server instances (RTMP, WHIP, ...)
4. Construct Runtime(config, servers)
5. return runtime.Start()
```

`main` does not own the media graph directly after bootstrap.

## Runtime Start Order

`Runtime::Start()` runs this sequence:

```text
1. ApplyManifest(bootstrap initial_manifest)     # kBootstrap mode
2. RuntimeHttpServer::Start()                  # HTTP thread
3. StartServerThreads()
     for each server:
       - server_thread  → server->Start()
       - watcher_thread → loop NextSource() → source_queue_.Push()
4. Main runtime loop (same thread as step 1 caller):
     while SourcePtr s = source_queue_.Pop():
       HandleSource(s)
5. JoinThreads()                               # watchers + server runners
6. CloseActiveState()
7. return exit code
```

Until a publisher connects, **no sink or decoder buffers fill**. Servers listen;
desired routes wait in `state_.desired`.

## Multi-source graphs **(target)**

When the manifest declares multiple `source_id` values (e.g. `live/studio` and
`live/guest`), each publisher connects independently:

```text
Watcher → source_queue → HandleSource(live/studio) → start A routes
Watcher → source_queue → HandleSource(live/guest)  → start B routes
```

Per source, under `state_.desired`:

- remux sinks filtered to that `source_id`
- decoders bound to that `source_id`
- composes whose `inputs` reference those decoders or that source (transitively)

Compose nodes that **fan in** decoders from **two sources** (e.g. `layout-a`
reading studio watermark + guest video) wire only when **both** sources are
active — strict upsert rejects the manifest otherwise.

Shared encoders (`enc-live`, `enc-record`) start when their compose inputs are
live; encode sinks start with encoder subscriptions.

See `docs/scenarios/complex_two_source.md`.

## Publisher Connects — What Starts

When a watcher delivers a new `SourcePtr`, `HandleSource()`:

```text
1. Validate source_id against state_.desired
2. Reject duplicate or unknown source_id → source->Close()
3. Register state_.active_sources[source_id]
4. Start routes in order (under state_.mutex):
     a. sinks     → StartRouteLocked()      # subscribe + SinkHandle::Start()
     b. decoders  → StartDecoderRouteLocked()
     c. composes  → StartComposeRouteLocked()  # wire each compose inputs[] **(target)**
     d. encoders  → StartEncoderRouteLocked()  **(target)**
```

### Direct remux branch (current default)

```text
StartRouteLocked:
  subscription = source->Subscribe()
  sink_handle = new SinkHandle(source, subscription, spec)
  sink_handle->Start()   # spawns HlsSinkRunner thread
```

Sink thread then:

```text
WaitReady(format)
loop subscription->Read(packet)    # target: ReadPacket
  write HLS
```

Data flow begins when **both** ingest pushes packets **and** sink thread pulls.

### Encode branch (target)

When encoder is in the graph:

```text
1. Construct Decoder + Compose objects (no Start) from manifest inputs[]
2. Wire pull endpoints: each compose resolves inputs[] to decoder/compose readers
3. encoder->Start()                    # encoder pulls compose(s) listed in its inputs
4. For each sink on encode branch:
     subscription = encoder->Subscribe()
     sink_handle->Start()              # sink thread reads encoded only
```

Start order for a full encode path:

```text
source registered → decoder wired → compose graph wired → encoder Start → sink Start(s)
```

Sinks on the encode branch **never** subscribe directly to source.

## Buffer Fill Order

Buffers are **demand-driven**. Nothing pre-fills the full pipeline at start.

### Direct remux

```text
Publisher sends data
  → ingest session Publish() → source rings + timeline

Sink thread ReadPacket
  → subscription cursor advances (may skip by filter)
  → returns next packet
  → HLS write
```

Fill propagates **only when sink pulls**. Slow sink → source rings grow →
backpressure on ingest.

### Encode branch (target)

```text
Sink thread:
  encoder_subscription.ReadPacket()
    ← encoded queue (encoder thread fills)

Encoder thread loop:
  compose.ReadPacket()                 # may pull a chain of composes
    ← compose buffer (fills from inputs on demand)
  encode → push encoded queue

Compose ReadPacket (called from encoder or another compose reader):
  pull each entry in inputs[] (decoder and/or upstream compose)
  ← local buffer per extended pull policy

Decoder ReadPacket (called from compose):
  source_subscription.ReadPacket()
    ← source timeline / rings
```

### Extended pull-based fill policy

When `ReadPacket` runs on decoder or compose and buffered count
`< buffer_threshold` (packet count, default example 50):

```text
repeat until threshold met or upstream empty:
  with probability burst_probability (example 50%): pull 2 upstream packets
  else: pull 1 upstream packet
return one packet to caller
```

First fill happens on the **first** `ReadPacket` at each layer, triggered by
the reader above (encoder thread, downstream compose, or terminal compose).

## Runtime Graph Update (`ApplyManifest`)

HTTP `POST /upsert_manifest` or bootstrap calls `ApplyManifestWithMode`.

### Validation

```text
1. Build new_desired from manifest
2. Validate (strict mode: referenced sources must be active)
3. On failure: reject entire manifest, keep previous state
```

### Reconcile order (inside lock)

```text
1. Compute routes to remove (sinks, encoders, composes, decoders whose spec changed or absent)
2. Close compose subtrees whose inputs reference removed nodes **(target)**
3. state_.desired = new_desired
4. Start new routes for already-active sources:
     a. new sink routes      → StartRouteLocked()
     b. new decoder routes   → StartDecoderRouteLocked()
     c. new compose routes   → StartComposeRouteLocked()   # topo order by inputs **(target)**
     d. new encoder routes   → StartEncoderRouteLocked() **(target)**
```

### Close removed routes (outside lock, after state update)

```text
1. CloseRouteState()          per removed sink (joins sink thread)
2. CloseEncoderRouteState()   per removed encoder **(target)**
3. CloseComposeRouteState()   per removed compose (readers stopped first) **(target)**
4. CloseDecoderRouteState()   per removed decoder
```

**Update principle:** tear down removed/changed downstream routes before relying
on new desired state; create new routes only for sources already active.
Sources that are not yet published remain in `desired` only.

### Bootstrap vs strict upsert

| Mode | Source validation |
|------|-------------------|
| `kBootstrap` (CLI / initial) | May declare routes before publisher connects |
| `kStrictUpsert` (HTTP) | Manifest sources must already be active |

## Runtime Close Order

Two entry points: `Runtime::Close()` (signal / destructor) and normal exit after
`Start()` returns.

### `Runtime::Close()` sequence

```text
1. HTTP server Close()              # stop new manifest requests
2. source_queue_.Close()            # unblock source loop
3. For each server: server->Close()
4. CloseActiveState()
```

`Close()` does **not** join server/watcher threads; that happens when `Start()`
returns to the main loop after `JoinThreads()`.

### `CloseActiveState()` sequence

Handles are moved out of maps under lock, then closed **without** holding
`state_.mutex`:

```text
1. Close all SinkHandle           # joins sink runner threads
2. Close all ComposeHandle
3. Close all DecoderHandle
4. Close all SourcePtr
5. Clear active route maps
```

**Target addition:** close **Encoder** handles (join encoder thread) after
compose, before sources — encoder sits between compose and sinks on the encode
branch.

### Recommended full shutdown (design intent)

Matches `docs/runtime.md`:

```text
http → freeze reconcile → close routes/subscriptions → sinks (+ encoders target)
  → servers → drain sources → join watchers/server threads → clear state
```

Short rule: **outputs stop pulling before ingest stops producing.**

```text
Sink/encoder threads stop
  → decoder/compose buffers drain (no threads)
  → source subscriptions close
  → ingest sessions end
  → source Close()
```

## Startup vs Shutdown Symmetry

| Phase | Order |
|-------|-------|
| **Start graph on source accept** | per source: remux sinks → decoders → composes (topo) → shared encoders when inputs ready **(target)** |
| **Stop graph** | encode sinks → encoders → composes (reverse topo) → decoders → remux sinks → sources **(target)** |
| **Manifest remove** | stop readers first; close nodes no longer referenced **(target)** |

## Legacy vs Target

| Area | Current | Target |
|------|---------|--------|
| Decoder | `DecoderRunner` thread, push `OnVideoFrame` to compose | Extended pull-based, `ReadPacket`, no thread |
| Compose | `IDecodedVideoConsumer` callback; `decoder_id` in manifest | Extended pull-based; `inputs[]`; fan-out via subscriptions |
| Sink input | Source subscription (encoded) | Encoder subscription on encode branch; source on remux branch |
| Encoder | Not integrated | Thread + queue + subscriptions |

Until migration completes, execution descriptions for decoder/compose follow the
**current** column at runtime; buffer-fill and threading sections above
describe the **target** column.

## Assumptions

- One `Runtime` instance per process
- Servers are created at startup from process config, not from manifest
- Each sink route has its own runner thread
- Each encoder has its own runner thread **(target)**
- Decoder and compose have no `Start()` and no runner thread **(target)**
- Compose nodes declare `inputs` only; graph is a DAG **(target)** — see `docs/compose.md`
- Buffers do not pre-fill at component construction; first pull starts the chain
- Manifest updates are full-replace; partial merge is deferred
