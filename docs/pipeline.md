# Pipeline Design

This note captures threading, pull models, and read APIs for the `argus` media
graph between source and sink.

Related docs:

- `docs/execution_model.md` — startup, threads, buffer fill, close order
- `docs/source.md` — source rings, shared timeline, subscriptions
- `docs/redesign.md` — component abstractions and typed ports
- `docs/final_design.md` — typed graph, abstract streams, sync ownership
- `docs/result.md` — composer and encoder timing policy

## Pipeline Branches

### Direct remux (current milestone)

Encoded samples from a publisher pass through source only:

```text
ingest thread → Source → Sink thread (ReadPacket) → HLS / file
```

The sink pulls encoded packets from a source subscription and writes output.

### Encode / compose branch (target)

Raw processing feeds a threaded encoder; sinks only consume encoded output:

```text
ingest thread → Source
                    ↓ ReadPacket (extended pull-based)
                 Decoder
                    ↓ ReadPacket (extended pull-based)
                 Composer
                    ↓ raw ReadPacket (any non-encoded media_type)
                 Encoder thread → encoded queue + subscriptions
                    ↓ ReadPacket
                 Sink thread(s) → HLS / file
```

The encoder owns encode/remux into encoded packets. Sinks on this branch only
**read encoded packets** from an encoder subscription and write them out.

## Thread Ownership

| Component | Background thread | Lifecycle |
|-----------|-------------------|-----------|
| Ingest session (RTMP/WHIP) | Yes — one per publisher connection | Session close |
| `Source` | No — passive storage | Close with session |
| `Decoder` | No | `Close()` only (no `Start()` for now) |
| `Composer` | No | `Close()` only (no `Start()` for now) |
| `Encoder` | Yes — pulls raw, encodes, fills output queue | `Start()` / `Close()` |
| `Sink` | Yes — one per output route | `Start()` / `Close()` |

Rules:

- **N streams inside one source does not mean N threads.** One ingest thread
  publishes all `stream_id` values into the source.
- **Decoder and composer have no lifecycle beyond `Close()`** for now.
- **Encoder is the sync + fan-out hub** on the encode branch: it reads multiple
  raw inputs at different rates and serves multiple sink subscribers.

## Read API

All component boundaries use **`ReadPacket`** (same name across stages; payload
interpretation depends on domain: encoded vs raw).

| From | `ReadPacket` delivers |
|------|------------------------|
| Source subscription | Encoded (or raw ingest) stream sample |
| Decoder | Raw sample (for example decoded video frame domain) |
| Composer | Raw sample (any non-encoded `media_type`) |
| Encoder subscription | Encoded packet for mux/write |

Push is used only at boundaries: **ingest → source**, **sink → file/network**.

## Extended Pull-Based Components

**Extended pull-based** describes `Decoder` and `Composer` (and the encoder's
raw-input side conceptually, but the encoder's runner thread performs that pull).

Properties:

- **No background thread**
- **Internal packet-count buffer** with a low threshold
- **Downstream `ReadPacket` triggers upstream fill** — demand propagates lazily
- **No `Start()`** — construct, use, `Close()` when done

### Buffer fill policy

Each extended pull-based component configures:

- `buffer_threshold` — packet count (default example: **50**)
- `burst_probability` — chance to over-fill when under threshold (default example:
  **50%**)

On each downstream `ReadPacket`:

1. If buffered packet count `< buffer_threshold`, pull from upstream.
2. Normally pull **one** upstream packet per fill step.
3. When under threshold, with probability `burst_probability`, pull **two**
   upstream packets instead of one for that step.
4. Repeat until threshold satisfied or upstream empty/blocked.
5. Return one packet to the caller from the local buffer.

This is the **same mechanism** for decoder and composer.

Example (threshold = 50, burst = 50%):

```text
buffered = 48  →  ReadPacket called  →  maybe pull 2 upstream  →  return 1
buffered = 51  →  ReadPacket called  →  return 1 without upstream pull
```

Exact defaults are per-node configuration; the policy shape is fixed.

### Decoder constraints (current phase)

- Extended pull-based, **no thread**
- **Single downstream reader** — decoder fan-out is deferred
- Pulls encoded packets from source (filtered); outputs raw packets

### Composer constraints (current phase)

- Extended pull-based, **no thread**
- `ReadPacket` pulls from decoder (and eventually multiple raw inputs for
  multi-input compose)
- Multi-input presentation sync happens inside `ReadPacket` when pulling from
  faster/slower inputs (see `docs/result.md`)

## Encoder

The encoder is **not** extended pull-based. It is a **queue-based autonomous
component with a runner thread**.

### Responsibilities

- Pull **raw** samples from one or more upstream nodes (composer branches)
- Accept any raw `media_type` (video frames, text, location JSON, audio PCM,
  ...) — **never encoded input**
- Synchronize inputs that arrive at different rates
- Encode / remux into encoded output packets
- Maintain an **encoded output queue**
- Expose **subscriptions** so multiple sinks can `ReadPacket` independently

### Why encoder has a thread

- Multiple raw inputs with different cadence require continuous alignment
- Output must stay ahead of slow sinks without blocking the compose pull chain
- **Fan-out** to multiple sink subscribers shares one encode path

### Sink role on encode branch

Sinks **do not** encode. They:

1. Subscribe to encoder output
2. Run a pull loop: `ReadPacket` from encoder subscription
3. Write to HLS / file / network

## Pull Chain (encode branch)

The **sink thread** drives the chain on each output tick:

```text
Sink::Run:
  packet = encoder_subscription.ReadPacket()
  write(packet)

Encoder thread (background):
  pull raw from composer(s), sync, encode, push to encoded queue

Composer::ReadPacket (on demand from encoder thread):
  fill buffer from decoder per extended pull policy
  return raw packet

Decoder::ReadPacket (on demand from composer):
  fill buffer from source per extended pull policy
  return raw packet
```

Note: on the encode branch the **encoder thread** pulls from composer, not the
sink. The sink only reads encoded output. Decoder and composer are still
extended pull-based with fill-on-read when their `ReadPacket` is called.

## Synchronization Summary

| Layer | Sync role |
|-------|-----------|
| Source | PTS order in timeline only |
| Decoder | Decode only; no multi-input sync |
| Composer | Multi-input raw alignment inside `ReadPacket` |
| Encoder | Raw input sync + output cadence + encoded timestamps |
| Sink | Write encoded packets; light mux interleave if needed |

## Legacy Code

Current implementation still uses:

- `DecoderRunner` with a background thread
- `IDecodedVideoConsumer::OnVideoFrame` push callbacks into compose

Target implementation replaces this with extended pull-based `ReadPacket` chains
and encoder subscriptions. See `docs/redesign.md`.

## Open Questions

- Default `buffer_threshold` and `burst_probability` per node type
- Encoder subscription cursor model (same pattern as source subscriptions)
- When direct remux and encode branch coexist in one process, manifest routing

## Assumptions

- `ReadPacket` is the universal pull API name at component boundaries
- Extended pull-based nodes expose only `Close()` until lifecycle expands
- Decoder fan-out is out of scope for the first extended pull-based milestone
- Encoder fan-out to multiple sinks is in scope via subscriptions
- Encoder input is always raw domain; output is always encoded domain
