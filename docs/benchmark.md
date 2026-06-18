# Benchmark Plan And Competitor Baselines

## Purpose

This document turns the project research into a benchmark plan that can be used
to compare this runtime against the competitor set listed in `options.md`:

- GPAC
- libobs / OBS Studio
- MLT
- MediaMTX
- FFmpeg
- GStreamer

It also records the current external benchmark evidence that is useful, and
separates that from the measurements we still need to run ourselves.

## Assumptions

- There is no root `README` in this repository at the time of writing, so
  assumptions are recorded here instead of a root readme.
- The target product is not just an encoder wrapper. From `scene_spec.md` and
  `result.md`, the product shape is a live graph runtime with:
  - hot reload / warm swap
  - multi-input scenes
  - switching
  - scheduling
  - composition
  - inference branches
  - multi-output fanout
- Because of that product shape, there is no honest single benchmark that
  compares all competitors directly. Some are routers, some are encoders, some
  are scene compositors, and some are timeline frameworks.

## What Can Be Compared Fairly

Use four benchmark classes instead of one blended number:

1. Packet relay / remux
2. Single encode
3. Multi-input scene compose + encode
4. Hot-reload / graph mutation stability

Any competitor that does not natively support one class should be marked `N/A`
instead of forcing an unfair comparison.

## Competitor Positioning

### FFmpeg

- Best baseline for software encode / transcode throughput.
- Weak baseline for mutable live graph orchestration.

### GStreamer

- Best baseline for dynamic media graph execution.
- Strong for measuring graph overhead, queueing, latency, and composition.

### libobs / OBS Studio

- Best baseline for live scene composition and render-driven production.
- Best for GPU-rendered scene complexity comparisons.

### MediaMTX

- Best baseline for routing, proxying, recording, and protocol fanout without
  re-encoding.
- Not a meaningful baseline for scene encode cost by itself.

### GPAC

- Best baseline for packaging, muxing, DASH/CMAF, and FFmpeg-backed transcode
  workflows.
- Encode cost is largely inherited from FFmpeg when FFmpeg encoders are used.

### MLT

- Best baseline for timeline / editorial composition workloads.
- Encode cost is also largely inherited from FFmpeg.

## Current External Evidence

### FFmpeg / x264

The strongest public baseline I found is x264 / FFmpeg throughput rather than
"scene cost per scene" benchmarks.

- OpenBenchmarking's public `x264` benchmark describes the workload as a
  multi-threaded CPU video-encoding test and, for the 1080p Bosphorus input,
  shows a median result around `95 FPS` across public uploaded results.[1]
- The same page shows stronger desktop/server CPUs comfortably above real-time
  1080p encode, which makes it a useful lower-level encode baseline, but not a
  scene-composition benchmark.[1]
- OpenBenchmarking's `FFmpeg` benchmark uses modified `vbench` scenarios for
  video-as-a-service workloads and reports `FPS` across scenarios such as
  `Live`, `Upload`, `Platform`, and `Video On Demand`.[2]

Interpretation:

- Use FFmpeg as the reference for pure software encode efficiency.
- Do not use FFmpeg alone as the reference for live mutable graph semantics.

### MediaMTX

MediaMTX's own documentation makes the comparison boundary explicit:

- In non-reencoding mode, its scalability bottlenecks are "almost always"
  bandwidth-related rather than CPU/RAM related.[3]
- Its docs say that to change format, codec, or compression, you should use
  FFmpeg or GStreamer together with MediaMTX.[4]
- Its built-in performance page shows a sample heap profile of about
  `5145.10 kB` in-use memory and a tiny sample CPU profile, but those examples
  are documentation snapshots, not throughput benchmarks.[5]

Interpretation:

- MediaMTX should be benchmarked as a router / proxy / recorder baseline.
- It should not be treated as a scene-encoding competitor.

### OBS / libobs

OBS provides useful qualitative guidance but not a universal scene benchmark:

- OBS says CPU requirements vary considerably with encoder, resolution, FPS,
  and scene complexity.[6]
- OBS recommends hardware encoders for best performance because they move the
  encode workload off the CPU to dedicated GPU hardware.[7]
- The libobs graphics documentation shows that rendering is built around its
  graphics subsystem and shader-driven rendering model, which is why it is the
  right composition benchmark baseline rather than a pure CPU encode baseline.[8]

Interpretation:

- OBS is the right comparison point for scene complexity and compositor cost.
- OBS is not the right baseline for headless packet-domain routing.

### GStreamer

GStreamer exposes the right instrumentation, but I did not find a standard
official "CPU per encoded scene" benchmark corpus:

- GStreamer documents latency tracing at pipeline and element level.[9]
- GStreamer documents leak tracing for object lifetime debugging.[10]
- The `x264enc` docs explicitly warn that encoder latency can be much higher
  than simple queue defaults and can stall non-trivial pipelines.[11]

Interpretation:

- GStreamer is a strong benchmark target for graph overhead, latency, queueing,
  and composition behavior.
- You will still need to run your own benchmark suite for apples-to-apples
  numbers.

### GPAC

GPAC's current encoding docs make the dependency clear:

- GPAC can explicitly route encoding through FFmpeg-backed encoders, and the
  docs state that currently all audio and video encoders in GPAC use FFmpeg.[12]
- GPAC's FAQ also frames GPAC as closer to packaging/streaming while leveraging
  FFmpeg for encoding duties.[13]

Interpretation:

- Compare GPAC primarily on packaging / transmux / live packaging overhead.
- Treat its encode numbers as "FFmpeg plus framework overhead."

### MLT

MLT's own features page also makes the dependency boundary clear:

- MLT lists FFmpeg for decoding, encoding, and effects.[14]
- MLT also warns that CPU/GPU memory transfer in hwaccel scenarios is heavy,
  which matters for any future hybrid CPU/GPU benchmark design.[14]

Interpretation:

- Compare MLT on timeline-first composition workflows.
- Treat encode throughput as "FFmpeg plus framework overhead."

## Benchmark Classes

### Class A: Packet Relay / Remux

Goal:

- Measure the cheapest path for live transport and fanout.

Workload:

- 1 publisher -> 1 path -> N readers
- H.264 + AAC
- No decode
- No compose
- No inference
- No re-encode

Competitors:

- MediaMTX
- FFmpeg
- GStreamer
- GPAC
- This runtime once packet path exists

Primary metrics:

- CPU total
- RSS memory
- packets dropped
- max stable readers
- added end-to-end latency

### Class B: Single Encode

Goal:

- Measure software or hardware encode cost without scene composition noise.

Workload:

- 1 synthetic or file source
- 1080p30 and 1080p60
- H.264 output
- x264 `veryfast` and `medium`

Competitors:

- FFmpeg
- GStreamer
- GPAC
- MLT
- OBS
- This runtime

Primary metrics:

- CPU total
- RSS memory
- encode FPS / realtime factor
- dropped / late frames

### Class C: Two-Input Compose + Encode

Goal:

- Measure the minimum scene-composition workload that matches this project.

Workload:

- 2 live inputs
- side-by-side layout
- one direct audio, one muted
- 1080p30 and 1080p60 output

Competitors:

- OBS / libobs
- GStreamer
- MLT
- This runtime

Primary metrics:

- CPU total
- RSS memory
- output latency
- dropped / duplicated frames
- stable runtime over 30 minutes

### Class D: Four-Input Compose + Encode

Goal:

- Measure scene amplification and queue pressure.

Workload:

- 4 live inputs
- 2x2 layout
- mixed audio policy
- 1080p30 and optionally 4K30 output

Competitors:

- OBS / libobs
- GStreamer
- MLT
- This runtime

Primary metrics:

- CPU total
- RSS memory
- per-input lateness
- output jitter
- frame drops

### Class E: Hot Reload / Graph Mutation

Goal:

- Measure the feature that most competitors do not model directly.

Workload:

- start with 2-input scene
- live-edit geometry
- swap source URI
- add/remove output sink
- switch active scene

Competitors:

- This runtime
- GStreamer with application-managed pipeline rebuild
- MediaMTX hot reload only for config-side routing cases

Primary metrics:

- output continuity
- swap time
- lost frames
- crash / deadlock / leak incidence

## Required Metrics

For every run, capture:

- CPU `%` total process
- CPU-seconds per minute of media processed
- RSS memory
- peak RSS memory
- output FPS
- realtime factor
- end-to-end latency
- dropped frames
- duplicated frames
- startup time
- 30-minute stability result

Also normalize results in three ways:

1. `per output stream`
2. `per encoded scene`
3. `per routed stream`

That avoids mixing router-heavy and compositor-heavy competitors into one bad
number.

## Recommended Normalized Formulas

### Per Encoded Scene

Use this only when the workload actually includes a composed or selected scene.

```text
cpu_per_encoded_scene = total_cpu_percent / encoded_scene_count
rss_per_encoded_scene = rss_mb / encoded_scene_count
```

In most of these tests, `encoded_scene_count = 1`, so this number is not very
informative alone. Prefer pairing it with scene complexity.

### Per Input Contribution

Useful for compose workloads:

```text
cpu_per_input = total_cpu_percent / live_input_count
rss_per_input = rss_mb / live_input_count
```

### Per Output Contribution

Useful for fanout workloads:

```text
cpu_per_output = total_cpu_percent / output_count
rss_per_output = rss_mb / output_count
```

### Realtime Safety Margin

```text
realtime_safety_margin = measured_output_fps / target_output_fps
```

Interpretation:

- `< 1.0` = cannot sustain realtime
- `1.0 - 1.2` = fragile realtime
- `> 1.2` = healthy headroom

## Test Environment Rules

Use the same hardware and same OS for every competitor run.

Lock these variables:

- CPU governor / performance mode
- GPU model and driver
- resolution and FPS
- codec and preset
- bitrate
- GOP size
- input media
- output target
- duration

Also separate software and hardware encode runs. Mixing `x264` and `NVENC/QSV`
into one table will hide the actual tradeoff.

## Proposed Benchmark Matrix

| ID | Class | Inputs | Scene Type | Output | Codec | Preset | Duration |
| --- | --- | --- | --- | --- | --- | --- | --- |
| A1 | Packet relay | 1 | none | 1 reader | copy | n/a | 10 min |
| A2 | Packet relay | 1 | none | 16 readers | copy | n/a | 10 min |
| B1 | Single encode | 1 | none | 1080p30 | H.264 | veryfast | 10 min |
| B2 | Single encode | 1 | none | 1080p60 | H.264 | veryfast | 10 min |
| B3 | Single encode | 1 | none | 1080p30 | H.264 | medium | 10 min |
| C1 | Compose + encode | 2 | side-by-side | 1080p30 | H.264 | veryfast | 30 min |
| C2 | Compose + encode | 2 | side-by-side | 1080p60 | H.264 | veryfast | 30 min |
| D1 | Compose + encode | 4 | 2x2 | 1080p30 | H.264 | veryfast | 30 min |
| D2 | Compose + encode | 4 | 2x2 | 4K30 | H.264 | veryfast | 30 min |
| E1 | Hot reload | 2 | side-by-side | 1080p30 | H.264 | veryfast | 30 min |

## Competitor Scorecard Template

### A. Routing / Relay

| Competitor | Decode? | Encode? | Readers | CPU % | RSS MB | Latency Added ms | Stable? | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| MediaMTX | no | no | 1 |  |  |  |  | expected strongest |
| MediaMTX | no | no | 16 |  |  |  |  | bandwidth-bound case |
| GStreamer | no | no | 1 |  |  |  |  | |
| FFmpeg | no | no | 1 |  |  |  |  | |
| GPAC | no | no | 1 |  |  |  |  | |
| This runtime | no | no | 1 |  |  |  |  | |

### B. Encode Only

| Competitor | Resolution | Preset | CPU % | RSS MB | FPS | Realtime Margin | Stable? | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| FFmpeg | 1080p30 | veryfast |  |  |  |  |  | baseline |
| FFmpeg | 1080p60 | veryfast |  |  |  |  |  | baseline |
| GStreamer | 1080p30 | veryfast |  |  |  |  |  | |
| GPAC | 1080p30 | veryfast |  |  |  |  |  | FFmpeg-backed |
| MLT | 1080p30 | veryfast |  |  |  |  |  | FFmpeg-backed |
| OBS | 1080p30 | x264 veryfast |  |  |  |  |  | |
| This runtime | 1080p30 | veryfast |  |  |  |  |  | |

### C. Scene Compose + Encode

| Competitor | Inputs | Layout | Resolution | CPU % | RSS MB | Latency ms | Drops | Stable? | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| OBS | 2 | side-by-side | 1080p30 |  |  |  |  |  | likely strong GPU path |
| GStreamer | 2 | side-by-side | 1080p30 |  |  |  |  |  | |
| MLT | 2 | side-by-side | 1080p30 |  |  |  |  |  | |
| This runtime | 2 | side-by-side | 1080p30 |  |  |  |  |  | |
| OBS | 4 | 2x2 | 1080p30 |  |  |  |  |  | |
| GStreamer | 4 | 2x2 | 1080p30 |  |  |  |  |  | |
| MLT | 4 | 2x2 | 1080p30 |  |  |  |  |  | |
| This runtime | 4 | 2x2 | 1080p30 |  |  |  |  |  | |

### D. Hot Reload / Live Mutation

| Competitor | Change Type | Output Gap ms | Drops | Crash? | Leak? | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| GStreamer app-managed | geometry update |  |  |  |  | |
| GStreamer app-managed | source swap |  |  |  |  | |
| MediaMTX | config reload route change |  |  |  |  | limited scope |
| This runtime | geometry update |  |  |  |  | |
| This runtime | source swap |  |  |  |  | |
| This runtime | sink add/remove |  |  |  |  | |

## Initial Expectations

These are hypotheses, not measured results:

- MediaMTX should win routing / relay efficiency when no decode or encode is
  needed.
- FFmpeg should be the cleanest encode-throughput baseline.
- OBS should be strong on scene composition, especially when hardware encode
  and GPU rendering are used.
- GStreamer should be the strongest framework baseline for dynamic graph
  pipelines.
- GPAC and MLT should mostly differ from FFmpeg in orchestration overhead and
  workflow shape, not in raw codec speed.
- This runtime should only be judged "better" if it stays close enough to the
  low-level baselines while clearly winning on hot reload, graph mutation,
  continuity, and product semantics.

## Recommended First Round

If time is limited, run these first:

1. FFmpeg `1080p30` and `1080p60` single encode
2. MediaMTX `1 -> 16` relay without re-encode
3. OBS `2-input` scene compose + encode
4. GStreamer `2-input` compose + encode
5. This runtime on the same four workloads

That first round will already tell us:

- whether we are closer to router cost, encoder cost, or compositor cost
- whether our graph runtime adds acceptable overhead
- whether hot reload semantics justify the extra complexity

## Sources

1. OpenBenchmarking x264 benchmark:
   - https://openbenchmarking.org/test/pts/x264
2. OpenBenchmarking FFmpeg benchmark:
   - https://openbenchmarking.org/test/pts/ffmpeg
3. MediaMTX scalability docs:
   - https://mediamtx.org/docs/features/scalability
4. MediaMTX re-encoding docs:
   - https://mediamtx.org/docs/other/remuxing-reencoding-compression
5. MediaMTX performance docs:
   - https://mediamtx.org/docs/features/performance
6. OBS system requirements:
   - https://obsproject.com/kb/system-requirements
7. OBS hardware encoding guidance:
   - https://obsproject.com/kb/hardware-encoding
8. libobs graphics / rendering docs:
   - https://docs.obsproject.com/graphics
9. GStreamer latency tracer docs:
   - https://gstreamer.freedesktop.org/documentation/coretracers/latency.html
10. GStreamer leaks tracer docs:
   - https://gstreamer.freedesktop.org/documentation/coretracers/leaks.html
11. GStreamer `x264enc` docs:
   - https://gstreamer.freedesktop.org/documentation/x264/index.html
12. GPAC encoding docs:
   - https://github.com/gpac/gpac/wiki/encoding
13. GPAC FAQ:
   - https://gpac.io/faq/
14. MLT features page:
   - https://www.mltframework.org/features/
