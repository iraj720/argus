# Compose Graph

This note defines how **compose** nodes fit in the `argus` media graph: wiring,
threading, fan-in, fan-out, and concrete `compose_type` behavior.

Related docs:

- `docs/scenarios/complex_two_source.md` — full two-source worked example
- `docs/pipeline.md` — extended pull-based model, encoder branch
- `docs/execution_model.md` — startup, reconcile, close order
- `docs/result.md` — multi-input timing inside compose
- `docs/final_design.md` — typed ports and graph validation
- `docs/runtime.md` — manifest validation and reconcile

## Push vs Pull: Source, Decoder, Compose

These are **not** the same storage model:

| Component | Fill model | Fan-out |
|-----------|------------|---------|
| **Source** | **Push** — ingest session writes into rings + timeline | Subscriptions with independent cursors (`docs/source.md`) |
| **Decoder** | **Extended pull-based** — fills on downstream `ReadPacket` | **Subscriptions** — multiple compose/encoder readers, each with its own cursor |
| **Compose** | **Extended pull-based** — fills on downstream `ReadPacket`; the node does **not** push-update its output buffer on its own | **Subscriptions into a ring buffer** on compose output — same reader pattern, different retention shape than source |

A compose node **pulls** from its declared inputs when a reader calls
`ReadPacket` on its output subscription. It does not run a background thread and
does not proactively fill its output ring; demand from readers drives both
upstream pull and output publication into the ring.

Decoder fan-out is confirmed: multiple nodes may list the same decoder in
`inputs` and read independently via decoder output subscriptions.

## Decoders and `stream_type`

**One decoder node per `stream_type` that requires decode**, bound to a source.
This matches FFmpeg (separate `AVCodecContext` per stream) and Argus fan-out
(subscriptions on that decoder for many compose/encoder readers).

```text
Source (packets: video/h264, voice/aac, location/text, …)
  ├──► video-dec   stream_type: video   packet_type in: video/h264  → video/raw
  ├──► voice-dec   stream_type: voice   packet_type in: voice/aac   → voice/pcm
  └──► (no decode) location/text packets — read from source subscription
```

| `stream_type` | Decoder? | Example `packet_type` in → out |
|---------------|----------|--------------------------------|
| `video` | Yes | `video/h264` → `video/raw` |
| `voice` | Yes | `voice/aac`, `voice/opus` → `voice/pcm` |
| `location` | Usually no | `location/text` — packet domain from source |
| `text` | Usually no | `text/plain` — packet domain from source |

Manifest (target):

```json
"decoders": [
  {
    "decoder_id": "video-dec-a",
    "source_id": "live/test",
    "stream_type": "video",
    "packet_type": "video/h264"
  },
  {
    "decoder_id": "voice-dec-a",
    "source_id": "live/test",
    "stream_type": "voice",
    "packet_type": "voice/aac"
  }
]
```

- **`stream_type`** selects the decode path (graph node kind).
- **`packet_type`** on the decoder narrows which encoded packets it consumes from
  the source (and defines codec).
- Multiple composes reading **`video/raw`** from `video-dec-a` do **not** create
  more video decoders — subscription fan-out only.

Future: compose `inputs` may include `kind: source` for packets that do not
require decode (`location/text`, `text/prompt`, ...). See
`docs/scenarios/complex_two_source.md`.

## Compose Is Extended Pull-Based

Every compose node is **extended pull-based** (same family as `Decoder`):

| Property | Behavior |
|----------|----------|
| Thread | **None** |
| Lifecycle | Construct → `ReadPacket` / `Close()` — no `Start()` |
| Internal buffer | Packet-count buffer + threshold/burst fill policy (upstream side) |
| Output fan-out | Ring buffer + subscriptions; filled when readers pull |
| Demand | A reader's `ReadPacket` triggers upstream fill lazily |

Universal boundary API: **`ReadPacket`**.

See `docs/pipeline.md` for buffer fill policy details.

## Abstract Type, Concrete Implementations

`Compose` is the abstract graph node. **`compose_type`** selects the
implementation (`jpg_snapshot`, `side_by_side`, `clip_prompt`, ...). Concrete
types live under `core/compose/<type>/`.

Each type declares:

- allowed **input kinds** (`source`, `decoder`, `compose`)
- per-input **packet filter** (`stream_id`, `packet_type`, `stream_type`) — see below
- whether it **emits raw** on output `ReadPacket`
- its own **`params`** schema (not shared manifest fields)

### Per-`compose_type` params

Manifest fields after `compose_type` are **type-specific**, not a shared compose
schema. The runtime validates params per type.

| `compose_type` | Example params | Notes |
|----------------|----------------|-------|
| `jpg_snapshot` | `output_root`, `snapshot_interval` | Test/experimental; writes JPG on pull |
| `side_by_side` | `snapshot_interval` | Layout; raw out for readers |
| `clip_prompt` | `prompt`, `model_root`, `output_root`, `snapshot_interval` | Test/experimental; logs scores on pull |
| `reasoning` | TBD | **Future** — text/reasoning output from prompt + voice/metadata inputs |
| `video_change` | TBD | **Future** — alter video from text + A/V inputs |
| `watermark` | TBD | **Future** — overlay location/metadata on video |

There is no global `output_root` required for all composes — only types that
need it declare it in their param set.

## Input kinds

| `kind` | Reads from | Typical `packet_type` |
|--------|------------|------------------------|
| `source` | Source subscription (no decode) | `text/prompt`, `location/text` |
| `decoder` | Decoder output subscription | `video/raw`, `voice/pcm` |
| `compose` | Compose output subscription (ring buffer) | `video/raw`, `text/reasoning` |

```json
{
  "kind": "source",
  "id": "live/studio",
  "stream_id": "text/prompt",
  "packet_type": "text/prompt"
}
```

## Multi-track voice (one decoder, many `stream_id`s)

Two voice tracks on one source share **one `voice-dec`** per our rules. Inputs
differ by **`stream_id`**:

```json
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
}
```

Main program voice uses the same decoder with `"stream_id": "voice/main"`.
See `docs/scenarios/complex_two_source.md`.

## Input packet selection

Each entry in `inputs[]` declares **which packets** that input reads from the
upstream node (`docs/source.md`).

```json
{
  "compose_id": "layout-a",
  "compose_type": "side_by_side",
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
      "packet_type": "voice/pcm"
    }
  ],
  "snapshot_interval": 50
}
```

Examples:

- `packet_type: video/raw` — decoded video from the video decoder
- `packet_type: voice/pcm` — decoded audio from the voice decoder
- `packet_type: location/text` — from source (`kind: source`)
- `packet_type: text/prompt` — from source (`kind: source`)

Graph validation checks that the upstream node can supply the requested
`packet_type` (and optional `stream_id`).

## Compose chains

Composes may cascade: downstream composes list upstream composes in `inputs`.
Example chain (see `docs/scenarios/complex_two_source.md`):

```text
reasoning-a → reasoning-b → (text/reasoning)
video-change-a ← video/raw + voice/pcm + text/reasoning
watermark-a ← video-change-a + location/text (source)
layout-a (side_by_side) ← watermark-a + guest decoders
```

Text emitted by a reasoning compose is just another **`packet_type`** on that
compose's output ring (`text/reasoning` or similar).

## Wiring Rule: Inputs Only

**A node declares what it reads from. It never declares who reads from it.**

```json
{
  "compose_id": "layout-a",
  "compose_type": "side_by_side",
  "inputs": [
    { "kind": "decoder", "id": "decoder-a" },
    { "kind": "compose", "id": "overlay-b" }
  ]
}
```

Downstream nodes (another compose, an encoder, a terminal compose) list this
compose in **their own** `inputs`. Graph edges are discovered by reference, not
by upstream `downstream` fields.

```text
                         ┌──► snap-a (jpg_snapshot → ./frames)
decoder-a ──► layout-a ──┼──► score-a (clip_prompt → log)
                         └──► enc-a (encoder → sinks)
              overlay-b ─────► layout-a
```

## Fan-In

One compose may declare **multiple** entries in `inputs`:

- `kind: source` — packets without decode (prompt, location, ...)
- `kind: decoder` — raw from a decode path
- `kind: compose` — raw or text from an upstream compose output

Multi-input presentation sync (presentation clock, stale/hold policy) runs
**inside that compose's `ReadPacket`**, not at every 1-in-1-out hop. See
`docs/result.md`.

Typical multi-input types: `side_by_side`, future scene/layout composes.

## Fan-Out

**Multiple readers may reference the same upstream decoder or compose** in their
`inputs`. Each reader gets an independent **subscription + cursor**:

- **Decoder output** — subscription fan-out (extended pull-based upstream)
- **Compose output** — subscription fan-out into a **ring buffer** on that compose node

The upstream node does not know how many readers exist and does not list them.
Compose output rings are **not** push-filled by a background thread; entries are
published when a reader's pull completes a processing step and the result is
committed to the ring.

This differs from **source** storage, where ingest **pushes** into rings and
timeline regardless of downstream demand.

## Outcomes (No Downstream Field)

| Outcome | How it is expressed |
|---------|---------------------|
| **Pass-through** | Type emits raw on output `ReadPacket`; other nodes reference this compose in `inputs` |
| **Terminal / side-effect** | Type-specific `params` (e.g. `output_root` for `jpg_snapshot`) — validated per `compose_type` |

There is **no** `downstream` field on any node. Test compose types (`jpg_snapshot`,
`clip_prompt`) are not normative for the long-term param model; they exist to
exercise the graph and pull path.

## Pull Through a Compose DAG

Demand starts at a **leaf reader** (encoder thread, terminal compose on pull,
etc.) and walks **up** the DAG:

```text
Encoder thread:  enc pulls layout-a.ReadPacket()
  layout-a (multi-input):
    pull decoder-a.ReadPacket()
    pull overlay-b.ReadPacket()
    sync inside ReadPacket → return raw

snap-a.ReadPacket():
  pull layout-a.ReadPacket()
  write JPG to output_root
```

Each hop uses the extended pull fill policy from `docs/pipeline.md`.

## Manifest Shape (Target)

```json
{
  "decoders": [
    {
      "decoder_id": "video-dec-a",
      "source_id": "live/test",
      "stream_type": "video",
      "packet_type": "video/h264"
    }
  ],
  "composes": [
    {
      "compose_id": "layout-a",
      "compose_type": "side_by_side",
      "inputs": [
        {
          "kind": "decoder",
          "id": "video-dec-a",
          "stream_id": "video/main",
          "packet_type": "video/raw"
        }
      ],
      "snapshot_interval": 50
    },
    {
      "compose_id": "snap-a",
      "compose_type": "jpg_snapshot",
      "inputs": [
        {
          "kind": "compose",
          "id": "layout-a",
          "packet_type": "video/raw"
        }
      ],
      "output_root": "./frames",
      "snapshot_interval": 50
    },
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
      "prompt": "a minion character",
      "output_root": "./infer",
      "model_root": "./models/mobileclip2_s2",
      "snapshot_interval": 50
    }
  ],
  "encoders": [
    {
      "encoder_id": "enc-a",
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

Notes:

- **`composes[].inputs`** replaces legacy **`decoder_id`** (current code still
  uses `decoder_id`; migration in progress).
- **Encoders and sinks** declare their own inputs (`encoder_id` on sink for
  encode branch — exact sink shape TBD; see `docs/runtime.md`).
- CLI bootstrap may still expand flat flags into this graph internally.

## Validation Rules

1. Every compose declares a non-empty `inputs` array.
2. **No node declares downstream.**
3. The compose reference graph must be a **DAG** (no cycles).
4. Each `inputs[].id` must reference an existing source, decoder, or compose in
   the manifest (per `kind`).
5. Fan-out is allowed: many nodes may reference the same upstream id.
6. Fan-in is allowed only for compose types that support multiple inputs.
7. Each `inputs[]` entry may declare `stream_id`, `packet_type`, and/or `stream_type` filters.
8. Params are validated **per `compose_type`** (no global required fields like
   `output_root` on every compose).

## Sync Ownership in a Compose DAG

| Hop | Sync |
|-----|------|
| 1-in-1-out compose | Transform only; no multi-input sync |
| Multi-input compose | Presentation clock and input alignment inside `ReadPacket` |
| Encoder | Raw input sync across its declared `inputs`, output cadence, encoded timestamps |

Do not re-sync at every chain stage. See `docs/result.md`,
`docs/final_design.md`, and `docs/scenarios/complex_two_source.md`.

## Relation to Current Code

Today's implementation (`core/compose/`, `core/runtime/`) still uses:

- `decoder_id` on each compose spec
- required `output_root` for every compose
- `IDecodedVideoConsumer::OnVideoFrame` push from `DecoderRunner`

Target: extended pull-based `ReadPacket`, `inputs[]` with stream filters,
decoder/compose output subscriptions (compose output uses ring buffer). See
`docs/execution_model.md` (Legacy vs Target).

## Open Questions

- Exact compose output ring retention and slow-reader drop policy (analogous to
  source ring drops — TBD).
- Whether encoder `inputs[]` entries use the same stream filter shape as compose
  (assumed yes).
- Future: nest type-specific params under a `params` object vs flat keys per type
  in JSON (flat keys OK for now; validation is per type).
