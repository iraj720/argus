# Events

This note defines **events** in `argus`: what they are, when compose components
raise them, and why they are central to the platform name.

**Scope:** design only. Event APIs and runtime wiring are not fully implemented.

Related design (read separately; this file does not modify them):

- `docs/source.md` — packets, timeline, sporadic metadata
- `docs/compose.md` — compose graph, extended pull-based processing
- `docs/scenarios/complex_two_source.md` — example pipeline

---

## Why “Argus”

In myth, **Argus Panoptes** was the many-eyed watcher — always observing, always
reporting. The engine name fits a system that does two things at once:

1. **Moves media** through a graph (sources, decoders, composes, encoders, sinks).
2. **Watches the graph** and **raises events** when something meaningful happens
   — not only when a frame is ready.

Events are how the runtime becomes **observable and reactive**: composes do not
just transform `video/raw` or `voice/pcm`; they **signal** detection, failure,
completion, policy changes, and operator-facing facts. External control planes,
 dashboards, automation, and downstream composes can subscribe without polling
every buffer.

Without events, `argus` would be a media pipe. With events, it is a **watched**
pipeline.

---

## Events vs media packets

Both are carried as **packets** on the shared model (`docs/source.md`), but they
serve different roles:

| | **Media packets** | **Event packets** |
|---|-------------------|-------------------|
| **Role** | Program content (A/V, location blobs, …) | Signals, observations, control facts |
| **`stream_type`** | `video`, `voice`, `location`, `text`, … | `event` |
| **`packet_type`** | e.g. `video/h264`, `voice/aac`, `location/text` | e.g. `event/reasoning`, `event/alert`, `event/metric` |
| **Cadence** | Steady or stream-like (fps, sample rate) | Sporadic — tied to conditions, not frame clock |
| **Typical path** | Decoders, composes, encoders | Composes (emit); subscribers (consume) |
| **Sync** | Co-timed with presentation clock in composes | Correlated by **timestamp**, not frame lockstep |

Events must **not** block the video pull chain. A compose raises an event as a
side effect of processing (or from an internal condition) while continuing to
honor extended pull-based `ReadPacket` semantics for media.

---

## Who raises events

**Primary emitters: compose components.**

Each `compose_type` may declare:

- **`evaluation_window`** — how many packets (or time span) form one observation window
- **`evaluation_interval`** — how often to run evaluation (every N packets or wall-clock step)
- **thresholds** — when a computed value crosses a limit, raise an event
- which **`packet_type`** each raised event uses
- type-specific **params** (model paths, sensor ids, formulas, …)

The **concrete `compose_type` implementation** decides **how to compute** the
value being compared to thresholds. The runtime only provides the windowing
schedule and event emission path — not the semantics of the metric:

| `compose_type` | Example computed value |
|----------------|------------------------|
| `clip_prompt` | Cosine similarity from inference model |
| `reasoning` | Confidence / directive stability over window |
| `watermark` | Location age or parse validity |
| Hypothetical `sensor_gate` | Max temperature read from metadata packets in window |
| Hypothetical `motion` | Simple math on frame deltas in window |

Inference, sensor reads, aggregates, and arithmetic are all **inside** the
compose type — Argus standardizes **when** to evaluate and **when** to raise,
not **what** formula or model to use.

Other nodes may emit events later (encoder: encode stall; sink: write failure;
source: publisher disconnect). **v1 design focus is compose-raised events.**

Decoders should stay decode-only and not own product-level events. Source ingest
may emit **session/lifecycle** events (connect, disconnect, format change) —
overlaps but distinct from **compose semantic** events documented here.

---

## Event packet shape (target)

Events reuse the universal **packet** unit:

```text
stream_id:     event/<compose_id>   or   event/<logical_channel>
packet_type:   event/<name>         e.g. event/reasoning_complete
stream_type:   event
pts:           correlation time (usually processing-time or aligned media pts)
payload:       structured body (JSON recommended in v1)
```

Example payload:

```json
{
  "event": "reasoning_complete",
  "compose_id": "reasoning-b",
  "severity": "info",
  "media_pts": 120040,
  "text": "Switch to wide shot when speaker says 'overview'",
  "confidence": 0.91
}
```

Payload schema is **per `packet_type`** / per compose contract, not one global schema.

When an event is threshold-driven, payload should include **evaluation metadata**:

```json
{
  "event": "clip_score_high",
  "compose_id": "score-a",
  "severity": "info",
  "media_pts": 120040,
  "evaluation_window": 50,
  "evaluation_interval": 10,
  "window_start_pts": 118000,
  "window_end_pts": 120040,
  "packets_in_window": 50,
  "metric": "cosine_similarity",
  "value": 0.94,
  "threshold": 0.85,
  "threshold_direction": "above"
}
```

Field names are illustrative; each `compose_type` documents its metrics and
threshold params.

---

## Evaluation model

Events are **not** raised on every single `ReadPacket` by default. They are
**evaluated on a window of packets**, on a fixed **evaluation interval**, and
**raised when a threshold passes**.

### Terms

| Term | Meaning |
|------|---------|
| **`evaluation_window`** | Number of packets (or equivalent span) that form one observation window — e.g. last 50 video frames, last 200 voice packets, last 30 location samples |
| **`evaluation_interval`** | How often to run evaluation — e.g. every 10 packets pulled, every 500 ms of presentation time, every 50th frame |
| **Metric** | Scalar or structured value **computed by the compose type** over the current window |
| **Threshold** | Compose-type param; when `metric` crosses it, emit an event with metadata |
| **Event** | `event/...` packet emitted **at evaluation tick** if threshold condition holds |

Window bounds may be expressed as **packet count** in v1; wall-clock windows are
allowed when a compose type documents PTS-based span.

### Evaluation loop (inside compose)

```text
On each ReadPacket (media path continues as today):
  append incoming packet(s) to internal window buffer
  if packets_since_last_evaluation >= evaluation_interval:
      metric = compose_type.compute(window)    # inference | sensor | math | ...
      for each configured threshold rule:
          if threshold_passed(metric, rule):
              emit event packet with metadata (value, threshold, window, pts, ...)
      advance evaluation cursor (sliding or tumbling window per type)
  return media output packet (if pass-through)
```

- **Sliding window** — drop oldest packets as new ones arrive (common for video).
- **Tumbling window** — clear window after each evaluation (common for batch scores).

The compose type chooses window semantics; manifest exposes `evaluation_window`
and `evaluation_interval` as **params** where applicable.

### Threshold rules

Each compose type defines its own threshold params, for example:

- `score_threshold: 0.85` — raise `event/clip_score_high` when similarity **above**
- `confidence_threshold: 0.7` — raise `event/reasoning_low_confidence` when **below**
- `temperature_max_c: 80` — raise `event/overtemp` when sensor max in window **above**

Multiple thresholds may map to **different** `packet_type`s. One evaluation tick
may emit zero, one, or many events.

### What the platform vs compose type owns

| Layer | Owns |
|-------|------|
| **Argus / runtime** | Pull path, window buffer hooks, interval scheduling, event ring fan-out |
| **`compose_type` implementation** | `compute(window)`, threshold interpretation, payload fields, inference/sensor/math |

---

## How compose raises an event

Raising is **threshold-driven** after **windowed evaluation**, not merely
“situation S on one packet”:

```text
Compose::ReadPacket():
  pull media / metadata inputs into window
  on evaluation_interval:
      metric = type-specific compute(window)
      if metric crosses threshold:
          append event packet (metadata includes value, threshold, window)
  return media output packet (if pass-through)
```

Properties:

- **Non-blocking** — event publish does not stall media upstream.
- **Same compose node** may emit **both** media (`video/raw`) and events
  (`event/...`) on different logical outputs (different `stream_id` / subscription filters).
- **Dedup policy** — compose types document whether the same threshold firing
  on consecutive intervals re-emits or suppresses until metric drops below (hysteresis).

Events from a compose use the same **output subscription + ring buffer** fan-out
model as media (`docs/compose.md`), unless a compose type is **event-only**
(side-effect / signal-only output).

---

## Situation catalog (examples)

Concrete events depend on `compose_type`, **evaluation_window**, **evaluation_interval**, and **thresholds**. Situations below are **when a threshold fires** after windowed evaluation — not necessarily every frame.

### `clip_prompt` (test type today)

| Event | Typical compute (type-owned) | Threshold example |
|-------|------------------------------|-------------------|
| `event/clip_score_high` | Mean/max cosine similarity in window | `score_threshold` 0.85 |
| `event/clip_score_low` | Same | below 0.3 |
| `event/snapshot_written` | N/A (action complete) | every `evaluation_interval` write |

### `reasoning` (future)

| Event | Typical compute | Threshold example |
|-------|-----------------|-------------------|
| `event/reasoning_complete` | Directive stable across window | stability ≥ param |
| `event/reasoning_failed` | Model error or timeout in window | any failure |
| `event/reasoning_low_confidence` | Inference confidence aggregate | below `confidence_threshold` |

### `video_change` (future)

| Event | Typical compute | Threshold example |
|-------|-----------------|-------------------|
| `event/video_change_applied` | Directive applied in window | success count > 0 |
| `event/video_change_rejected` | Policy check failed | any rejection |

### `watermark` (future)

| Event | Typical compute | Threshold example |
|-------|-----------------|-------------------|
| `event/location_stale` | Age of newest location in window | age > `stale_ms` |
| `event/watermark_applied` | Successful overlay in interval | applied count > 0 |

### Hypothetical sensor / math composes

| Event | Typical compute | Threshold example |
|-------|-----------------|-------------------|
| `event/overtemp` | Max sensor reading in window | > `temperature_max_c` |
| `event/motion_spike` | Pixel delta norm (simple math) | > `motion_threshold` |

---

## Who consumes events

Consumers declare interest like any other packet filter — **inputs only**, no
upstream `downstream` field:

| Consumer | Mechanism (target) |
|----------|-------------------|
| **Another compose** | `inputs[]`: `kind: compose`, `packet_type: event/...` |
| **Event sink** | Remux/log/metrics sink subscribing to `stream_type: event` |
| **Runtime / HTTP** | Control plane watches event stream (WebSocket, webhook — TBD) |
| **Operator UI** | Subscribes to `event/alert`, `event/reasoning_complete`, … |

Events can **drive graph behavior** without embedding control in media:

- `video_change` compose reads `event/reasoning_complete` text from `reasoning-b`.
- Automation reacts to `event/reasoning_failed` without decoding video.

Media and events from the same compose are **different subscriptions** on the
same node’s output rings, filtered by `packet_type` / `stream_id`.

---

## Manifest (target sketch)

Compose params include **evaluation** and **threshold** policy (per type):

```json
{
  "compose_id": "score-a",
  "compose_type": "clip_prompt",
  "inputs": [
    {
      "kind": "compose",
      "id": "layout-a",
      "packet_type": "video/raw"
    }
  ],
  "evaluation_window": 50,
  "evaluation_interval": 10,
  "score_threshold_high": 0.85,
  "score_threshold_low": 0.3,
  "prompt": "a minion character",
  "model_root": "./models/mobileclip2_s2",
  "output_root": "./infer"
}
```

```json
{
  "compose_id": "reasoning-b",
  "compose_type": "reasoning",
  "inputs": [
    {
      "kind": "compose",
      "id": "reasoning-a",
      "packet_type": "text/reasoning"
    }
  ],
  "evaluation_window": 30,
  "evaluation_interval": 5,
  "confidence_threshold": 0.7,
  "emit_events": ["reasoning_complete", "reasoning_failed", "reasoning_low_confidence"]
}
```

```json
{
  "compose_id": "video-change-a",
  "compose_type": "video_change",
  "inputs": [
    {
      "kind": "decoder",
      "id": "video-dec-a",
      "packet_type": "video/raw"
    },
    {
      "kind": "compose",
      "id": "reasoning-b",
      "packet_type": "event/reasoning_complete"
    }
  ]
}
```

```json
{
  "sink_id": "events-log",
  "source_id": "live/studio",
  "output_root": "./events",
  "mode": "record",
  "inputs": [
    { "packet_type": "event/*" }
  ]
}
```

Exact JSON fields (`emit_events`, wildcard filters) are **TBD** — see open items.

---

## Delivery semantics

| Topic | Target rule |
|-------|-------------|
| **Ordering** | Events on a compose channel are ordered by `pts` / emission order |
| **Retention** | Compose event ring — bounded; slow consumer may drop or jump (policy TBD) |
| **Correlation** | Payload should carry `media_pts` or `stream_id` when tied to a frame |
| **Severity** | Optional `info` / `warn` / `error` in payload for filtering |
| **Lifecycle** | Events are ephemeral signals; durable audit = event sink or external bus |

Events are **not** a replacement for metrics/tracing (latency histograms, queue
depth). They are **semantic** — “what happened in the pipeline narrative.”

---

## Relationship to location and text packets

| Kind | Example | Emitter | Use |
|------|---------|---------|-----|
| **Location media** | `location/text` from publisher | Source (ingest) | Data — watermark input |
| **Text prompt** | `text/prompt` from publisher | Source | Data — reasoning input |
| **Reasoning output text** | `text/reasoning` | Compose | Data — drives `video_change` |
| **Event** | `event/reasoning_complete` | Compose | Signal — observation, automation, logging |

Rule of thumb:

- If it is **program or model output** consumed as **input to the next transform**,
  model it as **`text/...` or `location/...`** media packets.
- If it is **“something worth watching”** for operators or control logic,
  model it as **`event/...`**.

A single compose step may emit **both** (e.g. `text/reasoning` for downstream
processing and `event/reasoning_complete` for the control plane).

---

## Assumptions

- Events are evaluated on **`evaluation_window`** + **`evaluation_interval`**; emission when **thresholds** pass.
- **Metric computation** is entirely **`compose_type`-specific** (inference, sensor, math, …).
- Events use the same packet + timeline vocabulary as media where they enter
  source-visible storage; compose-local events may live on compose output rings
  until a dedicated **event bus** node exists.
- Compose remains extended pull-based for **media**; evaluation and event emission are
  **side effects** of pull-driven processing, not a second push thread per compose.
- Event payloads are JSON in v1 unless a compose type specifies otherwise.
- Wildcard subscription filters (`event/*`) are desirable for logging sinks.

---

## Open items (discuss)

1. **Window units** — `evaluation_window` as packet count only vs also ms/PTS span.
2. **Interval units** — packet count vs wall-clock vs presentation PTS step.
3. **Global event bus vs compose-local rings** — events on source timeline or compose output only.
4. **HTTP/Webhook delivery** — first-class event sink vs file log only.
5. **Hysteresis** — suppress repeated threshold events until metric recovers.
6. **Multi-input windows** — one window per input vs fused window for multi-input composes.
7. **Wildcard `packet_type` filters** — `event/*` syntax and validation.
8. **Session events from source** — separate from compose threshold events.

If you want to lock any of these down, update this file only in a follow-up pass.
