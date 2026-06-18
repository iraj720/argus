# Scene Spec v1

## Goal

This spec defines the input file for:

```sh
scene run --spec <file>
```

The runtime will:

- load the full spec file
- build a graph generation from it
- watch the file for changes
- validate and warm the new generation
- atomically swap to the new generation
- keep the last good generation alive if reload fails

This is a full-snapshot spec, not a patch format.

## Format

- Format: `YAML`
- Versioned: yes
- Reload strategy: full file reload
- Identity model: all reusable objects must have stable `id`

## Top-Level Schema

```yaml
version: 1

runtime:
  reload:
    debounce_ms: 250
    atomic_swap: true
    keep_last_good: true
    require_output_continuity: true

defaults:
  video:
    width: 1280
    height: 720
    fps: 30
    format: RGBA
  audio:
    sample_rate: 48000
    channels: 2
    layout: stereo

sources: []
nodes: []
scenes: []
programs: []
outputs: []
```

## Design Rules

### 1. `nodes` and `scenes` are different

- `nodes` are reusable processing blocks
- `scenes` are reusable editorial/program units

They must not be merged in v1.

### 2. Timeline is wall-clock based

Programs support wall-clock scheduling, so entries can start at times like `09:00:00`.

### 3. Reloads may trigger full warm-swap

Changes like:

- canvas size
- fps
- source URI/backend
- sink settings
- scene structure

may rebuild affected subgraphs and warm-swap them into service.

## Sections

## `runtime`

Runtime-only behavior. Not media graph content.

```yaml
runtime:
  timezone: Asia/Tehran
  reload:
    debounce_ms: 250
    atomic_swap: true
    keep_last_good: true
    require_output_continuity: true
```

Rules:

- `timezone` is used to interpret wall-clock schedule entries that do not carry an explicit offset.
- `debounce_ms` controls file watch coalescing.
- `atomic_swap` must remain `true` in v1.
- `keep_last_good` must remain `true` in v1.
- `require_output_continuity` means reloads must not tear down active outputs unless a valid warmed replacement is ready.

## `defaults`

Default media settings used when omitted elsewhere.

```yaml
defaults:
  video:
    width: 1280
    height: 720
    fps: 30
    format: RGBA
  audio:
    sample_rate: 48000
    channels: 2
    layout: stereo
```

## `sources`

External inputs.

```yaml
sources:
  - id: cam_a
    uri: rtmp://localhost/live/cam_a
    kind: av
    reconnect:
      enabled: true
      backoff_ms: 500
      max_backoff_ms: 5000
    buffering:
      latency_ms: 200
```

Fields:

- `id`: stable unique identifier
- `uri`: source location
- `kind`: `video`, `audio`, `av`, or `data`
- `reconnect`: optional reconnect behavior
- `buffering.latency_ms`: optional ingest latency hint

## `nodes`

Reusable processing blocks.

Supported v1 node types:

- `switch`
- `inference`

Notes:

- composition belongs in `scenes`, not in generic nodes
- packet switching is not required in v1
- v1 switches are scene-domain switches

### Switch Node

```yaml
nodes:
  - id: main_switch
    type: switch
    mode: scene
    inputs:
      - cam_a_full
      - split_view
    default_input: cam_a_full
```

Fields:

- `id`: stable unique identifier
- `type`: `switch`
- `mode`: must be `scene` in v1
- `inputs`: referenced scene IDs
- `default_input`: starting scene before timeline activation

### Inference Node

This is part of the spec now as a named reusable node, but implementation can initially validate and ignore unsupported model backends until the inference runtime exists.

```yaml
nodes:
  - id: face_meta
    type: inference
    input: split_view
    model:
      kind: metadata
      name: face_detector
    output:
      mode: metadata
```

Fields:

- `input`: referenced scene ID
- `model.kind`: `metadata` or `frame`
- `output.mode`: `metadata`, `frame`, or `overlay`

## `scenes`

Reusable editorial/program units.

Supported v1 scene type:

- `compose`

```yaml
scenes:
  - id: split_view
    kind: compose
    canvas:
      width: 1280
      height: 720
      fps: 30
    stale_policy:
      video: hold_last_frame
      audio: silence
      timeout_ms: 800
    layers:
      - source: cam_a
        x: 0
        y: 0
        width: 640
        height: 720
        z: 0
        audio:
          mode: mute
      - source: cam_b
        x: 640
        y: 0
        width: 640
        height: 720
        z: 0
        audio:
          mode: direct
```

Fields:

- `id`: stable unique identifier
- `kind`: must be `compose` in v1
- `canvas.width`, `canvas.height`, `canvas.fps`: output video contract for the scene
- `stale_policy.video`: `hold_last_frame` or `black`
- `stale_policy.audio`: `silence` or `mute`
- `stale_policy.timeout_ms`: how long to tolerate a stale layer before policy changes from live use to fallback behavior
- `layers`: ordered visual/audio inputs

### Layer Fields

- `source`: source ID
- `x`, `y`, `width`, `height`: placement
- `z`: z-order
- `audio.mode`: `direct`, `mix`, or `mute`
- `audio.gain`: optional, only valid with `mix`

## `programs`

Programs bind reusable scenes/nodes to wall-clock timeline behavior.

```yaml
programs:
  - id: morning_show
    root: main_switch
    schedule:
      timezone: Asia/Tehran
      entries:
        - at: "09:00:00"
          select: cam_a_full
        - at: "09:15:00"
          select: split_view
        - at: "09:30:00"
          select: cam_a_full
```

Fields:

- `id`: stable unique identifier
- `root`: referenced node ID, usually a switch node
- `schedule.timezone`: optional override for this program
- `entries`: ordered wall-clock events

### Schedule Entry

Fields:

- `at`: wall-clock time
- `select`: target scene ID connected to the root switch
- `transition`: optional

Example with transition:

```yaml
        - at: "09:15:00"
          select: split_view
          transition:
            type: cut
            duration_ms: 0
```

Transition rules for v1:

- only `cut` is required
- `duration_ms` must be `0` for `cut`

Time format rules for v1:

- accepted form: `"HH:MM:SS"`
- timezone resolved from `programs[].schedule.timezone`, else `runtime.timezone`
- entries are interpreted as daily schedule points

## `outputs`

Outputs bind a program to one or more sinks.

```yaml
outputs:
  - id: live_main
    program: morning_show
    encoding:
      preset: balanced
    continuity:
      keep_running_on_reload_error: true
    sinks:
      - id: live_rtmp
        type: rtmp
        uri: rtmp://localhost/live/out
      - id: archive_mp4
        type: file
        path: ./out/morning_show.mp4
```

Fields:

- `id`: stable unique identifier
- `program`: referenced program ID
- `encoding.preset`: `low_latency`, `balanced`, or `high_quality`
- `continuity.keep_running_on_reload_error`: must remain `true` in v1
- `sinks`: one or more sink definitions

Preset guidance:

- `low_latency`: fastest live setting with lower buffering and lower bitrate
- `balanced`: default live preset
- `high_quality`: slower encoder settings, higher bitrate, and deeper buffering

### Sink Types

Supported v1 sink types:

- `rtmp`
- `file`

RTMP sink:

```yaml
      - id: live_rtmp
        type: rtmp
        uri: rtmp://localhost/live/out
```

File sink:

```yaml
      - id: archive_mp4
        type: file
        path: ./out/morning_show.mp4
```

## Hot Reload Semantics

On file change:

1. Read the whole file.
2. Parse YAML.
3. Validate references and graph shape.
4. Build a new generation.
5. Reuse compatible objects by stable `id`.
6. Rebuild or warm-swap changed subgraphs as needed.
7. Start and warm the new generation.
8. Atomically swap if outputs are ready.
9. Drain and retire the old generation.
10. Keep the current generation if any step fails.

## Reuse and Warm-Swap Expectations

Likely reusable in place:

- schedule entry changes
- selected scene changes
- layer geometry changes if supported live by the compositor
- audio gain changes

Likely warm-swap:

- canvas size changes
- fps changes
- source URI changes
- sink target changes
- scene membership changes
- switch input list changes

Implementation note:

- v1 may choose warm-swap more often than strictly necessary for correctness
- correctness and continuity are more important than minimal reallocation in the first version

## Validation Rules

- `version` must equal `1`
- all `id` values must be unique within their section
- all references must resolve
- `programs[].schedule.entries` must be sorted by `at`
- scene canvas dimensions and fps must be greater than zero
- at least one output must exist
- at least one sink must exist per output
- `nodes[].type` must be supported
- `scenes[].kind` must be supported
- no cycles are allowed in references
- `transition.type` must be `cut` in v1
- `audio.mode` must be `direct`, `mix`, or `mute`
- `audio.gain` is allowed only when `audio.mode` is `mix`

## Non-Goals for v1

- packet-domain switching
- arbitrary transition engines
- full NLE editing model
- patch-based spec updates
- distributed execution
- generalized plugin loading from spec

## Example

See [scene.example.yaml](/Users/nimarafieimehr/gibical/cgo/scene.example.yaml).
