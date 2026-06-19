# Scenario: Two-Source AI Layout Pipeline

End-to-end **target** graph exercising multi-source ingest, remux record sinks,
four decoders, a compose DAG with `kind: source` inputs, dual encoders, and
fan-out sinks.

Related docs:

- `docs/source.md` — packet model, multi-track voice
- `docs/compose.md` — inputs-only wiring, compose types
- `docs/pipeline.md` — remux + encode branches together
- `docs/runtime.md` — manifest validation
- `docs/execution_model.md` — multi-source lifecycle

**Status:** design / manifest target only — not fully implemented in runtime code.

## Story

Two ingest servers accept two independent publishers:

- **Source A** (`live/studio`) — main studio feed with video, voice, location,
  an instruction microphone, and a text user prompt.
- **Source B** (`live/guest`) — WebRTC guest with video and voice.

Before decode, each source gets **one remux record sink** that muxes the encoded
streams you select (e.g. video + voice) into a **single** HLS output — not one
sink per `packet_type`. Four decoders decode A video, A voice (both mic tracks
via `stream_id`), B video, and B voice.

On source A, **prompt** and **instruction mic** feed a **reasoning** compose
chain. Decoded main video + voice + reasoning text drive a **video_change**
compose. That output is **watermarked** with location metadata. **Side-by-side**
layout combines the processed studio branch with decoded guest WebRTC. Two
encoders produce two outputs; three sinks subscribe (one + two fan-out).

## Packet inventory

### Source A — `live/studio`

| `stream_id` | `packet_type` | `stream_type` | Decode? |
|-------------|---------------|---------------|---------|
| `video/main` | `video/h264` | `video` | Yes → `video/raw` |
| `voice/main` | `voice/aac` | `voice` | Yes → `voice/pcm` |
| `voice/instruction` | `voice/aac` | `voice` | Same voice decoder; filter by `stream_id` |
| `meta/location` | `location/text` | `location` | No — read from source |
| `text/prompt` | `text/prompt` | `text` | No — read from source |

### Source B — `live/guest`

| `stream_id` | `packet_type` | `stream_type` | Decode? |
|-------------|---------------|---------------|---------|
| `video/main` | `video/h264` | `video` | Yes → `video/raw` |
| `voice/main` | `voice/opus` | `voice` | Yes → `voice/pcm` |

(WebRTC may use `voice/opus`; decoders declare `packet_type` explicitly.)

## Decoders (four total)

One decoder per **`stream_type` per source** that requires decode — not one
decoder per packet track.

| `decoder_id` | `source_id` | `stream_type` | `packet_type` in → out |
|--------------|-------------|---------------|-------------------------|
| `video-dec-a` | `live/studio` | `video` | `video/h264` → `video/raw` |
| `voice-dec-a` | `live/studio` | `voice` | `voice/aac` → `voice/pcm` |
| `video-dec-b` | `live/guest` | `video` | `video/h264` → `video/raw` |
| `voice-dec-b` | `live/guest` | `voice` | `voice/opus` → `voice/pcm` |

Instruction mic (`voice/instruction`) and main voice (`voice/main`) share
**`voice-dec-a`**. Composes select track via `stream_id` on the input.

Prompt and location **do not** get decoders.

## Remux record sinks (before decode)

**Two** sinks on the **direct remux branch** — one per source. Each sink
**remuxes** multiple encoded inputs from that source into **one** output (HLS
file/playlist). The sink declares which packet streams to include via **`inputs`**.

| `sink_id` | `source_id` | `inputs` (mux into one record) | Role |
|-----------|-------------|--------------------------------|------|
| `rec-a` | `live/studio` | `video/h264`, `voice/aac` | Studio A/V record |
| `rec-b` | `live/guest` | `video/h264`, `voice/opus` | Guest A/V record |

You may include **more or fewer** streams per sink (e.g. add location, omit
voice, or filter a single voice track with `stream_id`). The sink runner pulls
matching packets from a source subscription and **interleaves/muxes** them on
write — same remux role as today’s HLS sink, extended with explicit `inputs`.

Example (target):

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

Instruction mic (`voice/instruction`) is **not** in this example record — only
main program A/V. Add another `inputs` entry or a separate sink if you want it
recorded too.

See `docs/runtime.md`.

## Compose DAG

All composes declare **`inputs` only**. Downstream nodes reference upstream in
their own `inputs`.

### Chain on source A

```text
text/prompt (source)  ──┐
voice/instruction     ──┼──► reasoning-a ──► reasoning-b ──► text/output
(voice-dec-a)           │         │
                        │         └── (text packets on ReadPacket)
                        │
video-dec-a (main)  ────┼──► video-change-a ◄── voice-dec-a (voice/main)
                        │         │
location (source)   ────┼──► watermark-a
                        │         │
                        └─────────┴──► layout-a (side_by_side) ◄── video-dec-b
                                                                  voice-dec-b
                                                                        │
                    enc-live ◄── layout-a          enc-record ◄── layout-a
                       │                                │
                    sink-live                      sink-rec-a, sink-rec-b
```

| `compose_id` | `compose_type` | Purpose | Key inputs |
|--------------|----------------|---------|------------|
| `reasoning-a` | `reasoning` **(future)** | Prompt + instruction mic → intermediate reasoning | `kind: source` prompt; `kind: decoder` instruction `stream_id` |
| `reasoning-b` | `reasoning` **(future)** | Second reasoning stage | `kind: compose` `reasoning-a` → `text/reasoning` |
| `video-change-a` | `video_change` **(future)** | Alter video from text stream | `video/raw`, `voice/pcm`, `text/reasoning` |
| `watermark-a` | `watermark` **(future)** | Burn location metadata on video | `video/raw` from `video-change-a`; `kind: source` location |
| `layout-a` | `side_by_side` | Multi-panel scene | Processed studio + guest video (and optional guest voice) |

Existing types (`side_by_side`, `jpg_snapshot`, `clip_prompt`) are test
implementations. `reasoning`, `video_change`, and `watermark` are **planned**
`compose_type` values — params TBD.

### Encoders and encode-branch sinks

| Node | Pulls from | Sinks |
|------|------------|-------|
| `enc-live` | `layout-a` (`video/raw`) | `sink-live` (1 subscriber) |
| `enc-record` | `layout-a` (`video/raw`) | `sink-rec-a`, `sink-rec-b` (2 subscribers) |

Both encoders list `layout-a` in their own `inputs` — fan-out from one compose
output via subscriptions.

## Full pipeline (ASCII)

```text
Server RTMP ──► Source A (live/studio)
Server WHIP ──► Source B (live/guest)

Remux (encoded record, one muxed output per source):
  A ──► rec-a  (video/h264 + voice/aac)
  B ──► rec-b  (video/h264 + voice/opus)

Decode:
  A ──► video-dec-a, voice-dec-a
  B ──► video-dec-b, voice-dec-b

Compose (source A path):
  source prompt + voice/instruction ──► reasoning-a ──► reasoning-b
  video-dec-a + voice-dec-a + reasoning text ──► video-change-a
  video-change-a + source location ──► watermark-a

Layout:
  watermark-a + video-dec-b (+ voice-dec-b) ──► layout-a (side_by_side)

Encode:
  layout-a ──► enc-live  ──► sink-live
           └──► enc-record ──► sink-rec-a, sink-rec-b
```

## Illustrative manifest (target)

Abbreviated JSON — field names and `kind: source` are **target**; HTTP parser may
not accept all of this until migration completes.

```json
{
  "sources": [
    { "source_id": "live/studio" },
    { "source_id": "live/guest" }
  ],
  "sinks": [
    {
      "sink_id": "rec-a",
      "source_id": "live/studio",
      "output_root": "./record/studio",
      "mode": "record",
      "inputs": [
        { "packet_type": "video/h264", "stream_id": "video/main" },
        { "packet_type": "voice/aac", "stream_id": "voice/main" }
      ]
    },
    {
      "sink_id": "rec-b",
      "source_id": "live/guest",
      "output_root": "./record/guest",
      "mode": "record",
      "inputs": [
        { "packet_type": "video/h264", "stream_id": "video/main" },
        { "packet_type": "voice/opus", "stream_id": "voice/main" }
      ]
    },
    {
      "sink_id": "sink-live",
      "encoder_id": "enc-live",
      "output_root": "./out/live",
      "mode": "live"
    },
    {
      "sink_id": "sink-rec-a",
      "encoder_id": "enc-record",
      "output_root": "./out/record-a",
      "mode": "record"
    },
    {
      "sink_id": "sink-rec-b",
      "encoder_id": "enc-record",
      "output_root": "./out/record-b",
      "mode": "record"
    }
  ],
  "decoders": [
    {
      "decoder_id": "video-dec-a",
      "source_id": "live/studio",
      "stream_type": "video",
      "packet_type": "video/h264"
    },
    {
      "decoder_id": "voice-dec-a",
      "source_id": "live/studio",
      "stream_type": "voice",
      "packet_type": "voice/aac"
    },
    {
      "decoder_id": "video-dec-b",
      "source_id": "live/guest",
      "stream_type": "video",
      "packet_type": "video/h264"
    },
    {
      "decoder_id": "voice-dec-b",
      "source_id": "live/guest",
      "stream_type": "voice",
      "packet_type": "voice/opus"
    }
  ],
  "composes": [
    {
      "compose_id": "reasoning-a",
      "compose_type": "reasoning",
      "inputs": [
        {
          "kind": "source",
          "id": "live/studio",
          "stream_id": "text/prompt",
          "packet_type": "text/prompt"
        },
        {
          "kind": "decoder",
          "id": "voice-dec-a",
          "stream_id": "voice/instruction",
          "packet_type": "voice/pcm"
        }
      ]
    },
    {
      "compose_id": "reasoning-b",
      "compose_type": "reasoning",
      "inputs": [
        {
          "kind": "compose",
          "id": "reasoning-a",
          "packet_type": "text/reasoning"
        }
      ]
    },
    {
      "compose_id": "video-change-a",
      "compose_type": "video_change",
      "inputs": [
        {
          "kind": "decoder",
          "id": "video-dec-a",
          "stream_id": "video/main",
          "packet_type": "video/raw"
        },
        {
          "kind": "decoder",
          "id": "voice-dec-a",
          "stream_id": "voice/main",
          "packet_type": "voice/pcm"
        },
        {
          "kind": "compose",
          "id": "reasoning-b",
          "packet_type": "text/reasoning"
        }
      ]
    },
    {
      "compose_id": "watermark-a",
      "compose_type": "watermark",
      "inputs": [
        {
          "kind": "compose",
          "id": "video-change-a",
          "packet_type": "video/raw"
        },
        {
          "kind": "source",
          "id": "live/studio",
          "stream_id": "meta/location",
          "packet_type": "location/text"
        }
      ]
    },
    {
      "compose_id": "layout-a",
      "compose_type": "side_by_side",
      "inputs": [
        {
          "kind": "compose",
          "id": "watermark-a",
          "packet_type": "video/raw"
        },
        {
          "kind": "decoder",
          "id": "video-dec-b",
          "stream_id": "video/main",
          "packet_type": "video/raw"
        },
        {
          "kind": "decoder",
          "id": "voice-dec-b",
          "stream_id": "voice/main",
          "packet_type": "voice/pcm"
        }
      ]
    }
  ],
  "encoders": [
    {
      "encoder_id": "enc-live",
      "inputs": [
        {
          "kind": "compose",
          "id": "layout-a",
          "packet_type": "video/raw"
        }
      ]
    },
    {
      "encoder_id": "enc-record",
      "inputs": [
        {
          "kind": "compose",
          "id": "layout-a",
          "packet_type": "video/raw"
        }
      ]
    }
  ]
}
```

## Design decisions exercised

| Decision | How this scenario uses it |
|----------|---------------------------|
| Packet unit + `packet_type` + `stream_type` | Five packet types on A; two on B |
| One decoder per `stream_type` per source | 4 decoders, not 7 |
| Multi-track voice | `voice/main` + `voice/instruction` via `stream_id` on inputs |
| `kind: source` on compose inputs | Prompt and location without decode |
| Inputs-only wiring | Encoders/sinks reference `layout-a`; layout does not list them |
| Extended pull-based compose | Full DAG; encoder thread pulls leaves |
| Remux before decode | Two remux sinks (one per source); each muxes multiple `inputs` into one HLS output |
| Encoder + sink fan-out | `enc-record` → two sinks |

## Assumptions

- Both sources must be **active** before strict HTTP upsert accepts the full graph.
- `reasoning`, `video_change`, and `watermark` compose types are placeholders until implemented.
- Remux sink **`inputs[]`** lists which encoded streams to mux (target manifest shape).
- Multi-input sync for `layout-a` and `video-change-a` runs inside those composes' `ReadPacket`.

## Open items

- Exact output `packet_type` strings from reasoning composes (`text/reasoning` vs `text/plain`).
- Remux sink interleave policy when one input stream is slower than another.
