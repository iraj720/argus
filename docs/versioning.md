# Versioning

This document defines **product versions** for the `argus` C++ runtime (`core/`):
what each version **means**, what must work before the next version starts, and
where to read the detailed spec for each target.

**Not the same as:**

| Doc | Purpose |
|-----|---------|
| `docs/runtime_milestones.md` | **Implementation steps** — small mergeable patches inside a version |
| `docs/final_design.md` (Go/GStreamer) | Legacy **irajstreamer2** v1 — different stack |
| `docs/scenarios/*.md` | **Reference graphs** — may target a future version |

Design references (stable across versions unless a version doc says otherwise):

- `docs/source.md`, `docs/compose.md`, `docs/pipeline.md`, `docs/events.md`
- `docs/scenarios/complex_two_source.md`

---

## What a version number means

A **version** is a **contract milestone**:

1. **Manifest** — JSON fields the runtime accepts and validates for that version.
2. **Graph** — node kinds and wiring rules (sources, sinks, decoders, composes, encoders).
3. **Behavior** — threading, pull model, fan-out, events — as documented.
4. **Definition of done** — tests and scenario(s) that must pass.

Versions are **sequential**. Version *N* assumes version *N−1* works. Patch
work inside a version follows `docs/runtime_milestones.md` (or a version-specific
milestone section in `docs/vN.md`).

Optional future field:

```json
{ "manifest_version": 7, ... }
```

Whether the HTTP body carries an explicit `manifest_version` is **TBD** — see
`docs/v7.md` open items.

---

## Version ladder (target)

Rough map from **today’s code** toward **v7**. Numbers are **argus C++ runtime**
versions, not semver of a published library.

| Version | Headline | Manifest / graph capability | Scenario |
|---------|----------|-------------------------------|----------|
| **v1** | Runtime + remux | `sources`, remux `sinks` with `source_id`; multi-sink fan-out | Single source → HLS record/live |
| **v2** | HTTP control | `POST /upsert_manifest`, desired vs active, strict upsert | Multi-sink via manifest |
| **v3** | Packets + decode (legacy path) | `decoders[]` with `source_id`; composes with legacy `decoder_id` | Decode + terminal composes (push path) |
| **v4** | Compose graph | `composes[].inputs[]`; `kind: decoder\|compose`; extended pull target | Compose chain + fan-out |
| **v5** | Multi-source + typing | `stream_type`, `packet_type`; decoders per stream type; remux sink `inputs[]`; `kind: source` on compose inputs | Partial `complex_two_source` (record + decode, no encode) |
| **v6** | Encode branch | `encoders[]`; encode-branch sinks with `encoder_id`; encoder subscriptions | `complex_two_source` without events / without all compose types |
| **v7** | **Argus watches** | Everything in v6 + **events** (`evaluation_window`, `evaluation_interval`, thresholds); event consume via `packet_type: event/*` | **`complex_two_source` + events** — see `docs/v7.md` |

**Current code (approx.):** between **v2 and v3** — runtime, manifest, remux sinks, legacy decoders/composes; target docs describe v4–v7.

Detailed specs: **`docs/v7.md`** exists today. **`docs/v1.md` … `docs/v6.md`** are
optional; this table is the index until those files are written.

---

## How to write `docs/vN.md`

Each version file should be **short and testable**:

1. **Goal** — one paragraph.
2. **Depends on** — previous version(s).
3. **In scope** — manifest fields, node kinds, behavior.
4. **Out of scope** — defer to vN+1.
5. **Reference scenario** — link to `docs/scenarios/` or minimal inline graph.
6. **Definition of done** — checklist + test ideas.
7. **Implementation notes** — pointer to runtime milestones if useful.
8. **Open items** — decisions still needed.

Avoid duplicating full design prose — **link** to `source.md`, `compose.md`,
`events.md`, etc.

---

## v7 (current focus)

**v7** is the first version where the platform name is fully justified: the
**complex two-source pipeline** runs end-to-end and composes **raise threshold
events** on packet windows.

Read: **[`docs/v7.md`](v7.md)**

---

## Assumptions

- Versions describe the **C++ argus runtime**, not the Go scene rewrite in
  `docs/final_design.md`.
- Breaking manifest changes bump the version; migration shims (e.g. legacy
  `decoder_id`) are allowed within a version only when documented.
- A version is **done** when its definition-of-done checklist is green in CI,
  not when all future compose types exist.

---

## Open items

1. Explicit `manifest_version` in JSON vs implicit “runtime build supports up to v7”.
2. Whether to backfill `docs/v1.md`–`docs/v6.md` or keep only the ladder table.
3. Semver for releases (`argus 0.7.0` vs “manifest v7”) — naming TBD.
