# Argus

`Argus` is a media engine platform for orchestrating AI inference graphs, enabling modular, real-time processing and execution of AI-driven media workflows.

## Architecture

```text
argus/
  cmd/
  core/
    compose/
      common/
      jpg_snapshot/
      side_by_side/
      clip_prompt/
    decoder/
    servers/
    sources/
    sinks/
    tests/
  third_party/
    libdatachannel/
  tools/
```

## Standalone Dependencies

`argus` is intended to be movable as its own folder. Keep third-party code
inside this tree:

```text
argus/third_party/libdatachannel
```

If you currently have `libdatachannel` one level above this folder, vendor it:

```text
./tools/vendor_libdatachannel.sh
```

You can also pass a custom source path:

```text
./tools/vendor_libdatachannel.sh /path/to/libdatachannel
```

## Current Scope

- publish-only RTMP ingest server
- publish-only WebRTC WHIP ingest server
- one session per inbound publisher
- one source buffer per published stream, with one or more in-process HLS sink workers attached by the runtime
- incoming RTMP audio/video/data messages are reconstructed into an FLV byte stream and handed to the HLS sink
- incoming WebRTC media is depacketized in-process and handed to the HLS sink as packetized H264/Opus
- server orchestration, session management, and the HLS sink now build on the C++ stack, while selected low-level RTMP protocol helpers remain in C
- accepted sources now own the canonical packet buffer and expose subscriptions to downstream sinks
- decoders subscribe to sources independently and decode H264 video into in-process `VideoFrame` objects
- compose nodes bind to decoders; concrete compose types live in their own folders (`jpg_snapshot`, `side_by_side`, ...)
- `jpg_snapshot` writes periodic JPG stills; `side_by_side` clones each sampled frame into a simple two-panel scene; `clip_prompt` runs MobileCLIP2-S2 prompt scoring and logs cosine similarity

```text
Source ──► Decoder ──► Compose (jpg_snapshot) ──► ./frames/
              │
              ├──► Compose (clip_prompt) ──► ./infer/infer.log
              └──► Compose (side_by_side) ──► ./scenes/
   │
   └──► HLS Sink ──► ./record/
```

## CLI

The current runtime convention is:

```text
./argus --source "live/test" --sink ./record/
```

To keep the HLS output in live mode with a sliding playlist:

```text
./argus --source "live/test" --live --sink ./record/
```

To write one source to multiple HLS outputs:

```text
./argus --source "live/test" --sink ./record-a/ --sink ./record-b/
```

To mix record and live sinks on the same source:

```text
./argus --source "live/test" --sink ./record-a/ --live --sink ./live-b/
```

To decode a source and write every 50th decoded video frame as JPG:

```text
./argus --source "live/test" --decoder --compose ./frames/
```

`--compose` without `--decoder` auto-creates a decoder for the current source.

To record HLS and capture snapshots from the same source:

```text
./argus --source "live/test" --sink ./record/ --decoder --compose ./frames/
```

To write a side-by-side clone scene every 50 decoded frames:

```text
./argus --source "live/test" --decoder --compose-side-by-side ./scenes/
```

To score decoded frames against a text prompt with MobileCLIP2-S2 (logs score and latency):

```text
./tools/download_mobileclip2_s2_models.sh
./argus --source "live/test" --decoder --compose-infer-prompt "a minion character" ./infer/
```

To run JPG snapshots and prompt inference as separate compose graph nodes on one decoder:

```text
./argus --source "live/test" --decoder \
  --compose ./frames/ \
  --compose-infer-prompt "a minion character" ./infer/
```

Each compose node binds to the same decoder and runs independently.

Requires `brew install onnxruntime`. Model files are downloaded into `./models/mobileclip2_s2/`.

You can also run both ingest servers in one process:

```text
./argus --server both --rtmp-port 1935 --webrtc-port 8080 --source "live/cam-a" --sink ./record-a/ --source "live/cam-b" --sink ./record-b/
```

Notes:

- `--source` matches the accepted source id
- `--server both` starts RTMP and WHIP together
- each `--sink` attaches to the most recent `--source`
- repeated `--sink` flags after one `--source` create multiple HLS sinks for that source
- `--decoder` after one `--source` creates a decode-only route for that source
- `--compose` creates a `jpg_snapshot` compose route for the current decoder
- `--compose-side-by-side` creates a `side_by_side` compose route for the current decoder
- `--compose-infer-prompt "..." ./path/` creates a `clip_prompt` compose route using MobileCLIP2-S2
- multiple compose nodes may bind to the same decoder; the decoder dispatches each decoded frame to every compose consumer in parallel
- repeated compose flags after one `--decoder` create multiple compose routes for that decoder
- `--compose` without `--decoder` auto-creates a decoder for the current source
- repeated `--source` blocks define explicit source-to-sink routes
- source ids are protocol-independent logical stream ids such as `live/test`
- protocol remains separate metadata, so a logical source id can be searched across both RTMP and WHIP
- `--live` applies to the next `--sink` only; if no `--live` appears before a sink, that sink uses the default record mode
- the runtime HTTP control server binds to the same `--host` value and uses `--runtime-port` with a default of `8090`

## Runtime HTTP

The runtime exposes:

```text
POST /upsert_manifest
```

Example request body:

```json
{
  "sources": [
    { "source_id": "live/test" }
  ],
  "sinks": [
    {
      "sink_id": "record-a",
      "source_id": "live/test",
      "output_root": "./record-a",
      "mode": "record"
    },
    {
      "sink_id": "live-b",
      "source_id": "live/test",
      "output_root": "./live-b",
      "mode": "live"
    }
  ],
  "decoders": [
    {
      "decoder_id": "decoder-a",
      "source_id": "live/test"
    }
  ],
  "composes": [
    {
      "compose_id": "compose-a",
      "decoder_id": "decoder-a",
      "compose_type": "jpg_snapshot",
      "output_root": "./frames",
      "snapshot_interval": 50
    },
    {
      "compose_id": "scene-a",
      "decoder_id": "decoder-a",
      "compose_type": "side_by_side",
      "output_root": "./scenes",
      "snapshot_interval": 50
    },
    {
      "compose_id": "infer-a",
      "decoder_id": "decoder-a",
      "compose_type": "clip_prompt",
      "prompt": "a minion character",
      "output_root": "./infer",
      "model_root": "./models/mobileclip2_s2",
      "snapshot_interval": 50
    }
  ]
}
```

Notes:

- `upsert_manifest` is full-replace desired state
- the runtime class exposes `ApplyManifest(...)` as its public graph-update API
- the runtime HTTP path validates sink bindings against currently active sources
- the CLI bootstrap path still uses the same manifest-apply handler, but in bootstrap mode it may predeclare routes before a publisher connects