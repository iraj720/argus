# Media Pipeline Design Conclusions

## Purpose

This document captures the conclusions from the design discussion around a live media pipeline library that supports:

- multiple inputs and outputs
- packet-domain and raw-frame-domain processing
- live switching
- timeline-driven or manual control
- scene composition
- inference on raw frames
- live graph updates with minimal reallocation
- recording and live publishing fanout

It also compares the custom design against `FFmpeg`, `GStreamer`, `MLT`, and `libobs`, and summarizes what should be outsourced to existing libraries versus what should remain custom runtime logic.

## Assumptions

- Encoders and decoders use `FFmpeg` libraries.
- The application using the library decides whether a switch is driven by timeline semantics or by manual commands.
- The pipeline supports pipes carrying multiple logical streams, including at least audio and video.
- There are two switch types:
  - packet-domain switch
  - raw-frame/domain-aware switch
- Inputs and packet switches do not solve drift.
- Multi-input compose types own multi-input AV timing in the raw domain.
- The encoder can duplicate or reuse the latest frame to maintain output cadence.
- Graph updates are applied as atomic graph-generation changes.
- Resource exhaustion can be handled by the application layer if the library exposes enough resource information early.

## Problem Shape

The target system is not just a decoder/encoder wrapper. It is a live media runtime with:

- mutable graph topology
- fanout to record and live outputs
- switch/timeline semantics
- composition across multiple sources
- inference integrated into raw-frame processing
- explicit lifecycle and recovery behavior

This matters because most existing media libraries solve only part of the problem:

- codecs and formats
- transport and muxing
- pipelines and plugins
- scenes and rendering
- editorial timelines

None of them exactly matches the full product model out of the box.

## Proposed Design

### High-Level Model

The custom design is a hybrid runtime:

- inputs can behave pull-based
- packet-level routing can remain relatively cheap
- decode moves media into the raw domain
- multi-input compose owns raw-domain synchronization and layout
- inference runs in the raw domain
- encoder enforces final output cadence
- outputs are queue-based sinks

This is a reasonable structure for a live production backend.

### Main Runtime Domains

There should be a hard conceptual split between the following domains:

1. Packet domain
   - compressed packets
   - cheap compatible switching
   - minimal processing
   - no attempt to solve cross-source drift

2. Raw frame domain
   - decoded video/audio frames
   - scene composition
   - inference
   - overlays, transforms, mixing
   - clocked synchronization across inputs

3. Control domain
   - timeline changes
   - switch commands
   - graph diffs
   - health and restart events
   - resource reservations

These domains should not be blurred together.

### Core Building Blocks

- `Input`
  - source ingestion
  - protocol/container awareness through `FFmpeg`

- `Output`
  - file, RTMP, or other sink
  - bounded queue-based writing

- `Pipe`
  - carries one or more typed streams
  - must preserve stream metadata and identity

- `Multicaster`
  - fanout with bounded buffering and explicit timeout/drop policy

- `PacketSwitch`
  - compressed-domain switching between compatible packet streams
  - may rebase timestamps and mark discontinuity

- `FrameSwitch`
  - raw-domain switching
  - may use cached/last frames under explicit policy

- `Decoder`
  - packet to raw frames

- `Compose` (abstract; `compose_type` selects concrete behavior)
  - raw-domain timing authority for **multi-input** types
  - AV drift handling, input liveness, scene layout/composition
  - 1-in-1-out types transform only (no multi-input sync)
  - examples: `side_by_side`, `jpg_snapshot`, `clip_prompt` — see `docs/compose.md`

- `InferenceNode` (scene-spec concept; in C++ runtime, inference may be a
  `compose_type` such as `clip_prompt`)

- `Encoder`
  - final cadence enforcement
  - final output timestamps/fps shaping

## Correct Responsibility Split

The design becomes internally coherent with this ownership model:

- `Input` and `PacketSwitch` do not solve drift.
- `Compose` (multi-input types) is the presentation-clock owner for multi-input raw composition.
- `Compose` (multi-input types) decides how to align or hold each input.
- 1-in-1-out compose chain stages do not re-run multi-input sync.
- `Encoder` enforces output cadence and can duplicate frames if needed.
- `Output` only writes what upstream delivers, subject to queue policy.

This is much better than asking every stage to partially solve timing.

## Switching Model

Two switch implementations are the right choice.

### Packet Switch

Use for:

- low-cost switching
- single active program source
- compatible encoded sources
- cases where avoiding decode/re-encode is important

Constraints:

- codec compatibility
- profile/level compatibility
- extradata / SPS/PPS compatibility
- time base compatibility
- audio codec/layout compatibility if audio is included
- keyframe/GOP behavior

Risks:

- broken continuity if sources differ semantically
- hidden decoder or muxer resets
- discontinuities if rebasing is incomplete
- mismatched audio/video characteristics

### Frame Switch

Use for:

- arbitrary source switching
- incompatible sources
- switch decisions after decode
- integration with composition/inference logic

Tradeoff:

- more expensive
- more flexible
- easier to make behavior consistent

## Timing, Drift, and Stale Inputs

### Compose Timing

Multi-input compose types should:

- own a presentation clock for multi-input raw composition
- know the target output fps for their branch
- track liveness per input
- decide whether to wait, reuse the last frame, or mark input stale
- solve drift between multiple raw inputs

In `argus`, compose nodes are **extended pull-based** (no background thread). The
above policy runs **inside `ReadPacket`** when pulling declared `inputs`. See
`docs/pipeline.md` and `docs/compose.md`.

### Encoder Timing

The encoder should:

- accept already-timed raw packets from compose nodes listed in the encoder's `inputs`
- enforce final cadence on encoded output
- generate monotonic output timestamps
- reuse/duplicate the latest valid frame only under explicit policy
- synchronize multiple raw inputs arriving at different rates (runner thread)

On the encode branch, sinks read **encoded** packets from encoder subscriptions
only. See `docs/pipeline.md`.

The encoder is the primary multi-input sync authority on the encode branch for
raw-to-encoded conversion and output cadence.
### Stale Source Policy

Using the last frame is valid, but only with explicit state transitions:

- `healthy`
- `late`
- `stale`
- `failed`
- `restarting`

Recommended policy:

- short hold using last frame
- then black/slate for video and silence/mute for audio
- publish health metadata/event

Without this, freeze-frame failure will look like a healthy pipeline.

## Audio Must Be First-Class

Audio is not optional complexity. The system needs explicit policy for:

- audio continuity at switch points
- crossfade vs hard cut
- silence insertion
- resampling
- channel layout normalization
- audio/video sync
- drift correction across independent live inputs

If audio is postponed, the runtime contracts will likely need redesign later.

## Queueing and Backpressure

Every queue in the system needs explicit policy:

- capacity
- timeout
- block/drop behavior
- observability

Queue-based outputs and multicasters are workable only if a slow sink cannot silently poison the whole graph.

Short write timeout alone is not enough. The runtime still needs a defined answer for:

- what gets dropped first
- whether outputs can be isolated
- whether a recorder is allowed to lag while live output stays real-time

## Graph Updates

### Correct Model

Atomic graph-generation updates are the correct model.

The safe pattern is:

1. Build the new graph generation or affected subgraph.
2. Reuse compatible nodes where semantic state remains valid.
3. Warm new nodes until they can produce valid output.
4. Atomically switch the attachment point.
5. Drain, retire, or cancel old generation nodes.
6. Close old resources only after no live consumer can still reference them.

### Reconfiguration Classes

Each node should declare how configuration changes are handled:

- `reuse-in-place`
- `warm-swap`
- `rebuild-required`
- `retire`

Examples:

- selected input on a manual switch: often `reuse-in-place`
- queue size tuning: often `reuse-in-place`
- inference model change: often `warm-swap`
- decoder or encoder backend/codec change: usually `rebuild-required`
- incompatible composition contract change: often `warm-swap` or `rebuild-required`

### Why This Matters

If the graph engine only decides what to close, it is incomplete. It must also decide:

- what is safe to reuse
- what must be warmed before commit
- what stale internal state invalidates reuse

## Lifecycle Model

### Startup

Do not think of startup as "start every node at once."

The stronger model is:

- construct nodes
- connect edges
- validate configuration and resources
- move generation to `ready`
- arm sinks
- transition generation to `running`

Nodes may start concurrently under a graph-level barrier, but startup should still be generation-aware and dependency-aware.

### Shutdown

Shutdown should be hierarchical and dependency-aware:

- stop accepting new upstream work
- drain queues if needed
- detach downstream consumers
- close sinks and hardware resources
- retire generation state

### Node States

Each node should expose states similar to:

- `created`
- `ready`
- `starting`
- `running`
- `late`
- `restarting`
- `draining`
- `stopped`
- `failed`

## Failure Recovery

### Important Distinction

Not all failures are equivalent. Recovery scope should distinguish:

- source/network I/O failure
- node-local processing failure
- shared device/context failure

Restarting a single node is appropriate only for some cases.

### Device Loss

GPU/device loss is a shared-failure-domain problem, not always a node-local problem.

Examples:

- driver reset
- GPU crash
- context invalidation
- suspend/resume effects

Restarting only one affected node can reconnect it to a dead or corrupted device context. Recovery may need to recreate the whole device subtree or graph generation using that device.

## Resource Constraints

### Hardware Session Exhaustion

This means decode/encode hardware resources can fail to allocate even when theoretical throughput still looks available.

Possible causes:

- too many concurrent encoder/decoder sessions
- device-specific session limits
- memory pressure
- hardware backend constraints

If the application layer owns admission control, the library still needs to expose:

- resource requirements before commit
- allocation failures before partial activation
- current usage and failure reason

### GPU Memory Amplification

Pointer-based data flow reduces copies, but it does not remove memory amplification.

Memory can still grow because of:

- retained references keeping surfaces alive
- fanout causing multiple branches to pin the same frame
- format conversion surfaces
- hardware-context transfers
- queue depth multiplying live surfaces

The real control variables are:

- max live frames per branch
- reference ownership rules
- release timing
- conversion policy
- queue caps

## Frame and Packet Ownership

This needs explicit rules.

At minimum the runtime must define:

- who owns packet/frame references
- whether buffers are immutable once published
- when multicast fanout may retain a reference
- when a consumer must clone
- how long a frame may remain pinned
- how stale generations release retained buffers

Without this, "mostly pointers, not copies" turns into hidden retention bugs.

## Important Edge Cases

- packet switch between streams with different SPS/PPS or codec parameters
- rebased timestamps that still break muxers or downstream outputs
- switch in the middle of decoder reorder delay or GOP boundary
- stale source whose last frame is still cached and looks alive
- frozen video with drifting or missing audio
- inference branch lagging while composition branch stays real-time
- dynamic graph edit while old-generation queues still hold buffers
- reuse of nodes whose internal state is no longer semantically valid
- restart loops caused by shared device failure
- one output requiring archival correctness while another requires low latency
- mismatched input fps in a multi-input compositor
- backpressure from a recorder impacting live output

## Outsourcing Boundaries

### What Should Be Outsourced

`FFmpeg` is a strong choice for:

- protocol I/O
- demux/mux
- codecs
- bitstream handling
- resampling
- scaling
- standard filters

This is where it already solves a large amount of difficult and low-value infrastructure work.

### What Should Remain Custom

The custom runtime should own:

- graph generations and atomic diff application
- node lifecycle
- restart scope and supervision
- packet-switch vs frame-switch semantics
- stale-source behavior
- composition/inference coordination
- product timeline/control semantics
- resource policy and admission

These are the core product-specific behaviors.

## Comparison With Existing Libraries

### FFmpeg

Strengths:

- best low-level toolbox for protocol I/O, muxing, demuxing, codecs, filters, scaling, and resampling
- excellent place to outsource encode/decode and media-format headaches

Weaknesses:

- not a first-class dynamic application-owned media runtime
- graph mutation and product orchestration semantics are not the central abstraction

Use it when:

- you want maximum control over your runtime
- you want to keep media plumbing battle-tested

### GStreamer

Strengths:

- closest general-purpose match to the target system
- dynamic pipelines
- selector elements for switching
- compositor and aggregation primitives
- timed property control
- plugin/custom-element model
- broad media ecosystem

Weaknesses:

- still does not directly provide your exact graph-generation diff model
- still requires custom application logic for restart/resource policy
- timeline semantics are not identical to your product model unless you adopt additional layers like GES
- you will still write significant custom elements or managers

Use it when:

- you are willing to build deeply on its pipeline model
- you want a framework that already understands runtime media graphs

### MLT

Strengths:

- good fit for timeline/playlist/tractor/transition style systems
- closer to editorial/NLE mental models

Weaknesses:

- less aligned with a live mutable backend runtime with packet-domain switching and inference branches
- weaker fit for the custom orchestration model described here

Use it when:

- timeline-first editing semantics dominate the product

### libobs

Strengths:

- strong scene/rendering/live-production concepts
- mature around sources, filters, transitions, outputs, encoders

Weaknesses:

- more graphics/live-production oriented than general backend graph orchestration
- less natural fit for a headless, application-owned, dynamically mutated server runtime

Use it when:

- your product is primarily OBS-like live scene/rendering logic

## Can Any Existing Library Solve Everything?

No single library perfectly covers:

- atomic graph-generation diffing
- packet-switch and frame-switch as first-class product abstractions
- your exact live-edit control semantics
- inference-aware scheduling and stale-input policy
- your exact supervision/restart model

The closest broad framework is `GStreamer`, but it still requires significant custom logic.

The most pragmatic option for this design is likely:

- `FFmpeg` for codec/protocol/media plumbing
- custom runtime for orchestration, switching, composition semantics, and graph mutation

## How Extendable Is The Custom Design?

It is reasonably extendable if the contracts are formalized early.

Good extension areas:

- live switching
- multiview
- fanout recording and publishing
- headless production backends
- scene composition
- inference-driven overlays or metadata generation
- automated production logic

Weak extension areas unless more abstractions are added:

- full nonlinear editing
- arbitrary editorial timeline tooling
- every odd media edge case under the sun

The design is extendable only if these contracts are strict:

- typed stream and metadata model
- domain separation
- node lifecycle model
- graph-generation model
- stale-source model
- frame ownership model

## Recommended Direction

### Best-Fit Option

The best-fit option from the discussion is:

- build a custom runtime
- outsource encode/decode/mux/demux/protocol/filter basics to `FFmpeg`
- keep packet-domain and raw-domain switching separate
- let multi-input compose types own raw-domain sync
- let the encoder enforce final cadence
- use atomic graph generations for live updates

### Why

This gives:

- strong product control
- lower reinvention in codec/protocol plumbing
- enough flexibility for live edits, composition, and inference
- clearer ownership than forcing the whole product into someone else's abstraction

## Minimum Contracts To Define Before Implementation

1. Stream type system
   - packet/video/audio/data/metadata semantics

2. Clock model
   - source time, presentation time, output cadence

3. Stale/late/failure policy
   - how long to hold, when to blank, when to restart

4. Queue policy
   - capacity, timeout, drop strategy, isolation behavior

5. Graph update semantics
   - reuse, warm-swap, rebuild, retire

6. Node lifecycle
   - ready/start/running/draining/stop/fail/restart states

7. Resource policy
   - hardware session reservation and admission control

8. Buffer ownership rules
   - references, cloning, release timing

9. Audio policy
   - continuity, mix/cut, silence insertion, resample rules

10. Recovery scope
   - node vs subgraph vs device domain vs whole generation

## Final Conclusion

The custom design is viable and coherent if implemented with hard contracts around timing, lifecycle, graph generations, and ownership.

It is stronger than using `FFmpeg` alone because the product needs orchestration and live mutation semantics beyond codec and format handling.

It is more product-specific and potentially cleaner than forcing everything into `GStreamer`, `MLT`, or `libobs`, but only if the runtime semantics are rigorously defined early.

The biggest mistakes to avoid are:

- blurring packet-domain and raw-domain responsibilities
- leaving stale-source behavior implicit
- treating startup as unstructured "start everything"
- under-specifying queue/backpressure policy
- assuming pointer-based flow automatically solves memory pressure
- assuming node-local restart is enough for shared-device failures

If those are handled carefully, a custom runtime plus `FFmpeg` is a strong implementation path for this problem.
