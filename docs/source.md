# Source Design

This note captures the agreed design for `argus` source buffering, multi-stream
multiplexing, and subscriptions.

It applies to the C++ implementation under `core/sources/` and to ingest servers
under `core/servers/` that publish into a source.

Related docs:

- `docs/scenarios/complex_two_source.md` — multi-track source example
- `docs/pipeline.md` — threading, extended pull-based nodes, encoder branch
- `docs/execution_model.md` — startup, threads, buffer fill, close order
- `docs/final_design.md` — typed graph, abstract streams, sync ownership
- `docs/redesign.md` — component abstractions and typed ports
- `docs/runtime.md` — runtime orchestration and manifest routing

## Goal

A single accepted publisher session (RTMP, WHIP, ...) may carry many logical
streams:

- video
- one or more audio/voice tracks
- location metadata
- text/captions
- future custom payloads

The source must:

- buffer live ingest with bounded memory
- preserve per-sample timestamps
- expose a shared presentation timeline across stream types
- let multiple downstream consumers read at independent rates and filters
- **not** perform multi-input composition sync (that belongs to multi-input **Compose** types)

## Scheduling Model

```text
Ingest (push)  →  Source storage  →  Subscription ReadPacket (pull)  →  ...
```

| Layer | Direction | Responsibility |
|-------|-----------|----------------|
| Server / session | Push | Accept network data, assign PTS, publish into source |
| Source rings + timeline | Queue | Bounded storage, global presentation order |
| Subscription `ReadPacket` | Pull | Deliver next matching sample for this consumer |
| Downstream | Pull | See `docs/pipeline.md` (extended pull-based decoder/compose, encoder thread, sink thread) |

**N streams does not mean N threads.** One ingest thread per publisher session
pushes all `stream_id` values into the source.

Pull gives backpressure. Pull alone does **not** synchronize streams. Sync across
**multiple inputs** is owned by multi-input **Compose** nodes and **Encoder** on the encode branch.
Light interleave by timestamp for a **single publisher** can happen when walking
the shared timeline or at mux write time.

## Packet model and typing

The **unit** moving through source storage and the processing graph is a
**packet**.

Each packet carries:

| Field | Role | Examples |
|-------|------|----------|
| **`packet_type`** | Payload format (MIME-like, extensible) | `video/h264`, `voice/aac`, `location/text`, `text/plain` |
| **`stream_type`** | Coarse family for routing and decode boundaries | `video`, `voice`, `location`, `text` |
| **`stream_id`** | Logical track/channel within one publisher | `video/main`, `voice/en`, `meta/location` |
| **`pts` / `dts`** | Session-comparable timestamps | |
| **`payload`** | Opaque bytes (stored in a per-`stream_id` ring) | |

Rules:

- **`stream_type`** is derived from the **`packet_type`** prefix (`video/h264` →
  `stream_type` `video`; `voice/aac` → `voice`; `location/text` → `location`).
- **`stream_id`** distinguishes parallel tracks of the same `stream_type` (two
  voice tracks → two `stream_id`s, same `stream_type` `voice`).
- After decode, output packets use new **`packet_type`** values (e.g.
  `video/h264` → `video/raw`, `voice/aac` → `voice/pcm`).

Legacy docs/code may still say `media_type`; target name is **`packet_type`**.

### Worked example: source A in two-source scenario

Publisher `live/studio` carries five packet types on one timeline:

| `stream_id` | `packet_type` | Decoder? |
|-------------|---------------|----------|
| `video/main` | `video/h264` | Yes |
| `voice/main` | `voice/aac` | Yes (shared voice decoder) |
| `voice/instruction` | `voice/aac` | Same voice decoder |
| `meta/location` | `location/text` | No — compose reads via `kind: source` |
| `text/prompt` | `text/prompt` | No — compose reads via `kind: source` |

See `docs/scenarios/complex_two_source.md`.

## Abstract streams (storage view)

Each published sample is a **packet** tagged as above.

`SourceFormat` / track metadata evolves into a **stream bundle**: a catalog of
active `stream_id` + `packet_type` descriptors announced via `WaitReady`.

## Storage Model

### Per-stream fixed ring buffers

Each `stream_id` has its own ring:

```text
videoRing:  [v0][v1][v2]...
voiceRing:  [a0][a1][a2]...
locRing:    [l0][l1]...
textRing:   [t0][t1]...
```

- Rings hold payload and stream-local sequence numbers.
- Ring size is fixed; old slots are reused when retention policy allows.
- Each slot must carry a monotonic `ring_sequence` so timeline entries can
  detect stale handles after reuse.

### Shared general buffer (timeline)

The **general buffer** is a source-owned, append-only **presentation timeline**
of lightweight entries pointing into rings:

```text
timeline:
  video(0ms)   → v0
  loc(10ms)    → l0
  text(20ms)   → t0
  voice(25ms)  → a0
  video(33ms)  → v1
  ...
```

Each `TimelineEntry` contains:

- `pts` (primary sort key)
- `dts` (optional; needed for encoded mux/decode ordering)
- `stream_id`
- `packet_type`
- `stream_type` (redundant but stored for fast filter; must match `packet_type` prefix)
- `ring_slot`
- `ring_sequence`

Payloads are **not** duplicated in the timeline.

## Inserter Owns Order

The **publish path** (inserter) is solely responsible for global timeline order.

On every `Publish(sample)`:

1. Write the sample into `ring[stream_id]`.
2. Insert a `TimelineEntry` into the general buffer at the correct presentation
   position ordered by `pts`.

This applies equally to video, voice, location, text, and any future type.

### Invariants

```text
timeline[i].pts <= timeline[i+1].pts
```

When `pts` is equal, use a deterministic tie-break (for example: video before
audio, then lexicographic `stream_id`, then `ring_sequence`).

### Out-of-order arrivals

Network jitter may deliver samples out of arrival order. The inserter must still
place each entry at its PTS position (sorted insert), or hold briefly in a small
reorder window before insertion.

Subscribers must **not** re-sort. They trust timeline order.

### Optional base-stream barrier (live)

For live mode, an optional `base_stream_id` (default `video/main`) may define a
delivery watermark:

```text
deliver_through_pts = latest_base_pts - holdback_ms
```

A subscription may block `Read` until `entry.pts <= deliver_through_pts`. Record
mode can set `holdback_ms = 0`.

This is a **delivery policy** on read, not a second ordering mechanism.

## Subscriptions

A subscription is a **read-only view** of source storage.

### Subscriber contract

Subscribers may update only:

- `cursor` — next timeline index to examine
- optional per-stream ring cursors for `ReadStream`
- `filter` — which `stream_id` / `packet_type` / `stream_type` patterns to accept
- optional `barrier_pts` / holdback policy
- optional `last_delivered_pts` for metrics

Subscribers must **not**:

- insert into or remove from the timeline
- reorder the general buffer
- write into rings
- pop shared entries in a way that affects other subscribers

### Read modes

Primary API: **`ReadPacket`**. Legacy/alternate names may exist during migration.

**`ReadMultiplex()`** — walk the shared timeline:

```text
while cursor < timeline.size:
    entry = timeline[cursor++]
    if not filter.matches(entry):
        continue    # skip for this subscriber only
    return materialize(entry) from ring
```

**`ReadStream(stream_id)`** — bypass the timeline; advance only that stream's
ring cursor. Use for narrow consumers (for example video-only decoder).

### Filters

Example subscription filters:

| Consumer | Filter |
|----------|--------|
| HLS remux sink | `inputs[]`: `video/*`, `voice/*` muxed into one output |
| Video decoder | `stream_type` `video` or `packet_type` `video/h264` |
| Voice decoder | `stream_type` `voice` or `packet_type` `voice/aac` |
| Location consumer | `stream_type` `location` or `packet_type` `location/text` |
| Full tee | accept all |

### Cursor start policy

Open decisions (pick one per subscription kind at implementation time):

- **Live edge** — `cursor = timeline.size` at subscribe time (only new samples)
- **From beginning** — `cursor = 0` (DVR / record catch-up)
- **From keyframe** — scan forward to first video keyframe entry

## Retention and Trim

Only the **source** compacts storage.

```text
trim_index = min(all subscription cursors) - safety_margin
```

When `trim_index > 0`:

- drop `timeline[0 .. trim_index)`
- release ring slots no longer referenced by any remaining timeline entry **and**
  passed by all ring cursors that may still read them

The slowest subscriber pins the window. When lag exceeds policy, apply explicit
drop rules (for example drop non-base streams first, then fail the slow consumer).

## Source Output vs Downstream Ports

The source exposes one logical **encoded bundle** to the graph:

```text
Source
  └── out:encoded (multiplexed timeline + rings)
```

Downstream processing nodes expose **typed ports** (see `docs/final_design.md`):

- decoder: encoded `packet_type` in → raw `packet_type` out (one **decoder node per `stream_type`** that requires decode — see `docs/compose.md`)
- compose: raw `packet_type` in (from declared `inputs`), raw `packet_type` out
- mux sink: `encoded` in (multiplex or filtered)

Compose output fan-out uses subscriptions into a **ring buffer** on the compose
node (extended pull-based — not push-filled like source). Decoder output
fan-out uses subscriptions as well. See `docs/compose.md`.

The source does not create separate physical ports per `stream_id`. Graph edges
use stream filters or `ReadStream` for narrow wiring.

## What Source Does Not Do

- Multi-camera / multi-publisher alignment
- Drift correction across independent live inputs
- Presentation-clock ownership for composed scenes
- Output fps enforcement
- Frame hold / stale / slate policy

Those belong to `Compose` (multi-input types), `Encoder`, and mux sinks as described in
`docs/result.md`, `docs/compose.md`, and `docs/final_design.md`.

## Relation to Current Code

Today `BufferedSource` uses a single interleaved deque and per-subscription
sequence cursors (`core/sources/buffered_source.h`). Migration path:

1. Introduce `StreamId`, `packet_type`, `stream_type`, and ring storage per `stream_id`.
2. Add shared `timeline` insert on publish; inserter maintains PTS order.
3. Change subscriptions to timeline cursor + filter skip.
4. Keep `ReadStream` compatibility for video-only decoder path.
5. Retire single-deque model when tests cover multi-stream fanout.

## Open Questions

- Cursor start policy defaults for live vs record sinks
- Timeline window sizing: wall-clock ms vs base-stream packet count
- Exact drop policy when subscribers lag past ring capacity
- Whether WHIP/RTMP ingest normalizes all stream PTS to one session clock at
  publish time

## Assumptions

- All streams in one publisher session share a comparable PTS time base after
  ingest normalization
- Timeline entries are immutable once inserted; correction uses discontinuity
  flags and a new timeline segment policy (TBD)
- Narrow consumers may use `ReadStream` to avoid scanning skipped types in the
  shared timeline
- Multi-input sync is explicitly out of scope for source
