# Irajstreamer2 Final Design

## Goal

Rewrite `irajstreamer` as `irajstreamer2/` with:

- Go as the control plane
- GStreamer as the media engine
- cgo as the bridge to GStreamer/native code
- backward-compatible `scene` command names and flags
- no HLS in v1
- default selected audio source only in v1
- warm-swap-ready runtime design from day 1

The design must keep performance-critical media work out of Go, while keeping timeline, scheduling, scene management, and graph orchestration in Go.

## Scope Decisions

These are the explicit decisions agreed so far:

- v1 is **library first**
- v1 rewrites the **`scene` command only**
- backward compatibility means **command names and flags only**
- no HLS in v1
- audio behavior in v1 is **default selected source only**
- scene definitions are built from CLI into Go structs in v1
- later versions will load scene definitions from file and warm swap updates

## High-Level Architecture

The system is split into two layers.

### 1. Control Plane in Go

This layer owns:

- CLI parsing
- scene structs
- timeline and Unix-time scheduling
- graph planning
- graph diffing
- warm-swap orchestration
- health and state reporting
- future file watching and hot reload

### 2. Media Engine in GStreamer / Native Code

This layer owns:

- ingest
- decode
- scale and convert
- compose
- switch
- encode
- mux
- outputs

This split is the core principle of the rewrite:

- Go manages the graph and timing
- GStreamer executes the hot media path

## Core Position

The best split is **not** “everything hot written in custom C”.

The best split is:

- use existing GStreamer elements and plugins first
- keep media processing native
- keep control and orchestration in Go
- add custom native code only when profiling proves a real need

That means:

- hot media path stays native
- Go does not process frames directly
- Go does not own codec loops, compositor loops, or muxing loops

## Why GStreamer First

GStreamer is the right engine for this project because:

- it naturally models media as a graph
- it supports dynamic running-pipeline changes
- it has native concepts that map to the problem:
  - source branches
  - decode branches
  - selectors
  - compositor
  - encoders
  - muxers
  - sinks
- it is a stronger fit for warm live switching than FFmpeg

FFmpeg may still be a future backend candidate, but the first implementation should target GStreamer.

## Future Backend Replaceability

The system should be designed so that later a backend can be replaced by another implementation, including a possible FFmpeg-based implementation.

That is only realistic if the control plane depends on **our own abstractions**, not on GStreamer types or GStreamer-specific details.

### Control Plane Must Not Depend On

- `GstElement`
- pad names
- GObject property conventions
- caps strings everywhere
- GStreamer state machine details
- request-pad semantics

### Control Plane Should Depend On

- `SceneSpec`
- `GraphSpec`
- `NodeSpec`
- `EdgeSpec`
- `MediaType`
- `Runtime`
- `GraphHandle`
- `RuntimeState`

This makes the architecture backend-agnostic at the interface level, even if the first implementation is GStreamer-specific internally.

## Main Abstraction Decision

The core abstraction should **not** be a generic `Pipe`.

That design is too weak because it hides the media contract.

A connection is not just “some stream”. It carries a specific media type:

- encoded video
- encoded audio
- raw video
- raw audio
- metadata

So the core abstraction must be a **typed graph**.

## Recommended Core Abstractions

### MediaType

```go
type MediaType string

const (
    MediaEncodedVideo MediaType = "encoded_video"
    MediaEncodedAudio MediaType = "encoded_audio"
    MediaRawVideo     MediaType = "raw_video"
    MediaRawAudio     MediaType = "raw_audio"
)
```

In `argus`, encoded and raw ports carry **packets** tagged with `packet_type`,
`stream_type`, and `stream_id`. See **Packet model** in `docs/source.md`.

### NodeKind

```go
type NodeKind string

const (
    NodeInput      NodeKind = "input"
    NodeFanout     NodeKind = "fanout"
    NodeSwitch     NodeKind = "switch"
    NodeDecoder    NodeKind = "decoder"
    NodeComposer   NodeKind = "composer"   // C++ runtime: concrete Compose nodes with compose_type
    NodeEncoder    NodeKind = "encoder"
    NodeMux        NodeKind = "mux"
    NodeOutput     NodeKind = "output"
)
```

### Endpoint

```go
type Endpoint struct {
    Node NodeID
    Port PortID
}
```

### EdgeSpec

```go
type EdgeSpec struct {
    ID       string
    From     Endpoint
    To       Endpoint
    Media    MediaType
    Optional bool
}
```

### NodeSpec

```go
type NodeSpec struct {
    ID         NodeID
    Kind       NodeKind
    Name       string
    Properties map[string]any
}
```

### GraphSpec

```go
type GraphSpec struct {
    ID    GraphID
    Nodes []NodeSpec
    Edges []EdgeSpec
}
```

This graph is:

- visible to the Go control plane
- independent from GStreamer details
- expressive enough for warm-swap planning

## Control Plane Visibility Requirement

One concern was that the control plane should not be blind to the graph.

That requirement is correct.

The Go layer should clearly know:

- which input feeds which decoder
- which decoder feeds which compositor slot
- which branch feeds which selector
- which scene output goes to encoder, mux, and outputs

But Go should **not** own all low-level linking mechanics.

### Correct Balance

- Go owns graph structure and orchestration
- backend owns low-level media object wiring

So Go sees:

- decoder -> scale -> compositor -> encoder

without directly owning:

- GStreamer request-pad logic
- exact pad names
- low-level linking and cleanup details
- native state transition boilerplate

## Recommended Package Shape

```text
irajstreamer2/
  cmd/
  internal/
    app/
    media/
    planner/
    runtime/
    backend/
      gstreamer/
```

### `cmd`

- backward-compatible `scene` command parsing

### `internal/media`

- user-facing scene and graph model types

### `internal/planner`

- scene-to-graph planning logic

### `internal/runtime`

- active/staged graph lifecycle
- warm swap orchestration

### `internal/backend`

- backend-agnostic engine contracts

### `internal/backend/gstreamer`

- GStreamer implementation through cgo

## Backend-Agnostic Interfaces

### Scene Model

```go
type Canvas struct {
    Width  int
    Height int
}

type Layout struct {
    X            int
    Y            int
    Width        int
    Height       int
    ZIndex       int
    Transparency float64
}

type InputSpec struct {
    ID     string
    URI    string
    Layout Layout
}

type OutputSpec struct {
    ID  string
    URI string
}

type SceneSpec struct {
    ID          string
    Canvas      Canvas
    Inputs      []InputSpec
    Output      OutputSpec
    AudioSource int
    FPS         int
}
```

### Backend Engine Contract

```go
type Engine interface {
    Start(ctx context.Context) error
    Close() error

    BuildGraph(ctx context.Context, spec GraphSpec, opts BuildOptions) (GraphHandle, error)
    ActivateGraph(ctx context.Context, handle GraphHandle, opts ActivationOptions) error
    DestroyGraph(ctx context.Context, handle GraphHandle, opts DestroyOptions) error

    State(ctx context.Context) (RuntimeState, error)
}
```

### Runtime Controller Contract

```go
type Controller interface {
    RunScene(ctx context.Context, spec SceneSpec) error
    StageScene(ctx context.Context, spec SceneSpec) error
    ActivateStagedScene(ctx context.Context) error
    Stop(ctx context.Context) error
    State(ctx context.Context) (SceneState, error)
}
```

### Planner Contract

```go
type ScenePlanner interface {
    PlanScene(spec SceneSpec) (GraphSpec, error)
}
```

## Planner Role

The planner converts a user-facing `SceneSpec` into a backend-agnostic `GraphSpec`.

This keeps:

- CLI compatibility logic separate from runtime
- graph planning testable without cgo
- backend-specific logic out of the control plane

The planner is pure Go logic.

## Scene Graph Shape

Conceptually, a scene expands into a graph like:

```text
input -> decode -> transform/layout -> compositor
input -> decode -> audio-pick
compositor -> encoder -> mux -> output
audio-pick -> mux
```

For multiple inputs:

- each input gets its own source/decode/transform branch
- all video branches feed the compositor
- one selected input provides the default audio source in v1

## Runtime Design

The runtime should not be “start components manually in arbitrary order”.

The runtime should own graph lifecycle.

### Good Runtime Operations

- apply initial graph
- stage graph
- activate staged graph
- destroy old graph

### Bad Runtime Style

- manual `Start()` ordering for each node everywhere
- arbitrary node-level low-level rewiring in user-facing code

So the runtime should expose high-level graph lifecycle, not low-level media object lifecycle.

## Warm Swap Vision

Warm swap must be a first-class concept from day one.

### Meaning of Warm Swap

Warm swap means:

- build the next graph without cutting current output
- warm or preroll it until ready
- atomically switch output from active graph to staged graph
- drain and destroy the old graph safely

### Runtime State

The runtime keeps:

- one active graph
- zero or one staged graph

### Warm-Swap API

```go
type Runtime interface {
    ApplyInitialGraph(ctx context.Context, spec GraphSpec) error
    StageGraph(ctx context.Context, spec GraphSpec) error
    ActivateStaged(ctx context.Context) error
    RemoveOldGraph(ctx context.Context) error
}
```

## Warm Swap Patterns

There are two valid implementation patterns.

### Pattern A: Two Full Graphs

- active graph is running
- staged graph is built separately
- output route is switched atomically
- old graph is drained and removed

This is simpler for a first implementation.

### Pattern B: Stable Output Trunk

- encoder, mux, and sinks stay alive
- active and staged scene branches both feed a selector
- cutover is a selector switch
- old branch drains and is removed

This is the better long-term design.

### Recommendation

- v1 can start with Pattern A for simplicity
- mature runtime should move toward Pattern B

## Stable Trunk Principle

Not every part of the graph should be hot-swapped.

Warm updates should happen at safe boundaries.

### Good Hot-Swap Boundaries

- input source branch
- scene branch
- switch active input
- output destination branch

### Bad Hot-Swap Boundaries

- arbitrary node in the middle of encoder/mux chain
- unrestricted random graph surgery

The stable trunk principle means:

- keep the downstream output path as stable as possible
- swap scene or source branches upstream

This makes warm swaps safer and more predictable.

## Timeline Design

Timeline belongs in Go, not inside GStreamer.

The controller should own:

- Unix-time scheduling
- mapping time to desired scene or source selection
- initiating staged activation at the correct time

### Timeline Event Shape

```go
type SwitchEvent struct {
    At       time.Time
    TargetID string
}

type TimelineSpec struct {
    InitialTargetID string
    Events          []SwitchEvent
}
```

String timestamps like `"50.21"` are not recommended as the core model.

The controller can still parse CLI strings, but internal scheduling should use typed time values.

## Why Not Generic Pipe-Based Design

A generic design like:

- `NewPipe()`
- `NewInput()`
- `NewDecoder()`
- `NewOutput()`

has some good instincts, but it is too weak as the main architecture because:

- it hides media type contracts
- it becomes hard to validate connections
- it becomes hard to diff graphs safely
- it becomes hard to define safe hot-swap boundaries
- it leads to fragile imperative wiring

The correct replacement is not “more detailed pipes”.

The correct replacement is:

- typed nodes
- typed ports
- typed edges
- graph planning
- graph lifecycle runtime

## Inference / Non-Standard Components

Inference-like components should not be part of v1.

Reason:

- they have very different latency and readiness characteristics
- they may not behave like normal live media transforms
- they complicate scheduling and warm-swap semantics

The v1 rewrite should focus on:

- scene composition
- switching
- encoding
- RTMP output

Inference can be added later as a separate class of node once the core runtime is stable.

## Backend Replaceability Reality

The system should be designed so a future FFmpeg backend is possible.

However:

- backend replaceability is an architectural goal
- it is **not** a guarantee that every backend will support every advanced warm-swap feature equally well

GStreamer is naturally closer to the required runtime graph model.

FFmpeg may still be implemented later, but the control plane must remain independent from GStreamer specifics for that to be possible.

## Pseudocode: Main Flow

```go
func main() {
    opts := parseSceneCLI()
    scene := buildSceneSpecFromCLI(opts)

    engine := gstreamer.NewEngine()
    planner := planner.NewScenePlanner()
    controller := runtime.NewSceneController(engine, planner)

    ctx := signalContext()

    must(engine.Start(ctx))
    must(controller.RunScene(ctx, scene))

    <-ctx.Done()
    _ = controller.Stop(context.Background())
    _ = engine.Close()
}
```

## Pseudocode: Warm Swap Flow

```go
func (c *SceneController) UpdateScene(ctx context.Context, next SceneSpec) error {
    nextSpec, err := c.planner.PlanScene(next)
    if err != nil {
        return err
    }

    staged, err := c.engine.BuildGraph(ctx, nextSpec, BuildOptions{Warm: true})
    if err != nil {
        return err
    }

    if err := c.engine.ActivateGraph(ctx, staged, ActivationOptions{Atomic: true}); err != nil {
        return err
    }

    old := c.activeGraph
    c.activeGraph = staged
    c.activeScene = &next

    if old != nil {
        if err := c.engine.DestroyGraph(ctx, old, DestroyOptions{Drain: true}); err != nil {
            return err
        }
    }

    return nil
}
```

## Future File-Based Reloads

The design is intentionally shaped for future hot-reload from config files.

Later, when config is file-based:

- file watcher loads new desired scene spec
- controller computes the desired graph
- controller stages the next graph
- controller activates staged graph atomically
- old graph is drained and destroyed

The first implementation should keep this simple:

- stage whole new scene graph
- activate whole staged scene graph
- optimize to partial updates only later

## v1 Deliverable Summary

`irajstreamer2` v1 should provide:

- library-first core
- GStreamer backend through cgo
- backward-compatible `scene` command flags
- RTMP input support
- file input support
- RTMP output support
- scene composition with layouts
- default selected audio source
- Go-owned scheduling and orchestration
- staged/active graph lifecycle
- warm-swap-ready runtime design

## Assumptions

- backward compatibility in v1 means command names and flags only, not full internal behavior compatibility
- v1 rewrites only the `scene` command, not `switch` or `rawscene`
- v1 excludes HLS entirely
- v1 uses selected-source audio only and does not implement audio mixing
- v1 uses CLI-parsed Go structs as the scene model; config file hot reload comes later
- the first backend implementation is GStreamer, even though the control plane is designed to keep a future FFmpeg backend possible
- initial warm swaps may stage full scene graphs before later optimization toward stable-trunk branch swapping

---

## argus C++ Pipeline Model

This section records design decisions for the current `argus` C++ runtime
(`core/`), which implements ingest, buffering, decode, compose, and HLS output
without the Go/GStreamer stack described above. The same graph principles apply;
the implementation language and first backend differ.

See also:

- `docs/compose.md` — compose graph, inputs-only wiring, fan-in/fan-out
- `docs/scenarios/complex_two_source.md` — two-source stress case
- `docs/source.md` — source rings, shared timeline, subscriptions
- `docs/compose.md` — compose graph, inputs-only wiring, fan-in/fan-out
- `docs/pipeline.md` — threading, extended pull-based nodes, encoder branch
- `docs/execution_model.md` — startup, threads, buffer fill, close order
- `docs/redesign.md` — `Server`, `Source`, `Sink`, typed ports
- `docs/runtime.md` — manifest, routes, HTTP control
- `docs/result.md` — composer timing, queue policy, switching

### Abstract Streams Inside Typed Ports

The graph is still **typed**, not a weak generic pipe. A port declares the
**domain** and accepted media patterns. Inside an encoded port, many **abstract
streams** may flow in parallel:

```text
stream_id        packet_type       stream_type
video/main       video/h264        video
voice/en         voice/aac         voice
meta/location    location/text     location
text/captions    text/plain        text
```

Typed ports answer: “what **domain** is this connection?” (`Encoded`, `Raw`).
**`packet_type`** answers: “what format is this **packet**?” **`stream_type`**
answers: “which decode/route family?”

Graph validation checks port domain compatibility, `packet_type` compatibility,
and optional `stream_id` filters.

### Port and Connection Model

| Component | Typical ports | Threading |
|-----------|----------------|-----------|
| `Source` | `out:encoded` — multiplexed bundle | No (ingest pushes) |
| `Decoder` | `in:encoded packet_type`, `out:raw packet_type` | One node per `stream_type` per source; extended pull-based |
| `Compose` | `in:raw/*`, `out:raw/*` | Extended pull-based, no thread; inputs-only wiring |
| `Encoder` | `in:raw/*`, `out:encoded` | Runner thread; declares compose inputs in manifest |
| Mux sink | `in:encoded` | Runner thread, `ReadPacket` from encoder or source |

Rules:

- **Source** emits one encoded bundle; it does not expose a physical port per
  `stream_id`.
- **Encode branch:** compose DAG → `Encoder (thread) → Sink` — encoder remuxes/encodes;
  sink only reads encoded packets. Each compose/encoder declares **`inputs`** only.
  See `docs/compose.md` and `docs/pipeline.md`.
- **Direct remux:** `Source → Sink` for pass-through HLS without transcoding.
- **Narrow consumers** (decoder) use `ReadStream` or a stream filter on source.

### Scheduling and Backpressure

```text
Direct remux:  ingest (push) → Source → Sink thread (ReadPacket) → write
Encode branch: ingest (push) → Source → Decoder → Compose* → Encoder thread → Sink thread (ReadPacket) → write
```

- Ingest **pushes** into source storage.
- **Decoder** and **Compose** are **extended pull-based** — no background thread;
  `ReadPacket` triggers upstream fill per `docs/pipeline.md`.
- **Compose graph** supports chains, fan-in (multi-input types), and fan-out
  (multiple readers reference the same compose in their `inputs`). See
  `docs/compose.md`.
- **Encoder** has a runner thread, pulls raw, outputs encoded queue with
  subscriptions for multiple sinks.
- **Sink** has a runner thread; on the encode branch it only **reads encoded**
  packets from an encoder subscription.

Pull provides backpressure. See `docs/pipeline.md` for buffer threshold policy.

### Synchronization Ownership

Sync is **not** uniform across the graph. Each layer has a narrow job:

| Layer | Sync responsibility |
|-------|---------------------|
| `Source` | Store samples, PTS order in timeline, bounded retention |
| Subscription `ReadPacket` | Deliver next matching sample; no cross-stream alignment |
| `Decoder` | Decode only; extended pull-based, no multi-input sync |
| `Compose` (multi-input) | Multi-input raw alignment **inside `ReadPacket`** |
| `Compose` (1-in-1-out) | Transform only; no multi-input sync |
| `Encoder` | Raw input sync (different rates), output cadence, encoded timestamps; fan-out to sinks |
| `Sink` | Write encoded packets from encoder or source subscription |

Do not ask `Source` or `Read` to solve multi-camera drift. Do not ask mux sinks
to fix compose-level alignment.

#### Stream classes

**Co-timed media** (video, audio, voice): share session PTS; mux sink interleaves
on write; composer/encoder sync when multiple inputs or output cadence matter.

**Sporadic metadata** (location, text, inference): correlated by timestamp, not
frame lockstep; consumers attach events to the timeline without blocking video.

**Multi-publisher scenes**: multi-input **Compose** nodes own the presentation clock; sources only
provide per-publisher timelines.

### Source Summary

The source design is specified in `docs/source.md`. Key points:

- per-stream fixed rings for payload storage
- shared append-only presentation timeline (general buffer)
- inserter on publish maintains global PTS order across all stream types
- subscriptions own only `(filter, cursor)`; they never mutate shared storage
- `ReadMultiplex()` skips non-matching types; `ReadStream(id)` reads one ring

### Graph Validation

An edge is valid when:

1. Port domains match (`Encoded` → `Encoded`, `Raw` → `Raw`, ...).
2. The downstream node accepts the upstream `packet_type` / `stream_type`.
3. Optional `stream_id` filters are satisfiable from the upstream bundle.

Manifest routing uses **`inputs`** on compose and encoder nodes (consumer declares
upstream). See `docs/compose.md` and `docs/runtime.md`. Explicit port edge
objects may follow the `EdgeSpec` model later.

**Stress case:** `docs/scenarios/complex_two_source.md` — two sources, remux +
encode branches, four decoders, compose DAG, dual encoders, three encode sinks.

### Implementation Order (argus)

1. Reshape packet model toward `StreamSample` / stream bundle (alongside current
   `SourcePacket` during migration).
2. Implement rings + shared timeline in source (`docs/source.md`).
3. Add subscription filters and dual read modes.
4. Introduce typed port descriptors on processing nodes.
5. Extend manifest with compose `inputs[]`, encoder `inputs[]`, and stream filters.

### argus Assumptions

- first source implementation targets single-publisher sessions (RTMP/WHIP)
- timeline inserter normalizes PTS to one session clock at publish time
- universal pull API at component boundaries is `ReadPacket`
- decoder and compose are extended pull-based (no thread, `Close()` only)
- compose nodes declare `inputs` only; no upstream downstream fields
- encoder has a thread, raw in / encoded out, subscriptions for sink fan-out
- compose fan-out to multiple readers is in scope via subscriptions
- compose and encoder sync semantics follow `docs/result.md`, `docs/pipeline.md`, and `docs/compose.md`
- multi-input composition is out of scope for the source layer
