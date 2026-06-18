# Runtime Milestones

This document breaks the runtime work into small implementation steps so each stage can be built, tested, and reviewed without mixing unrelated changes.

The goal is accuracy and consistency, not speed through a large rewrite.

## Principles

Each milestone should:

- keep the system buildable
- keep ingest behavior working unless the milestone explicitly replaces it
- preserve the existing source and sink abstractions unless that milestone is specifically about changing them
- move orchestration out of `main` and into runtime in small steps
- include tests for the new responsibility introduced in that milestone
- avoid mixing manifest design, HTTP wiring, reconcile logic, and media behavior changes in one patch

## Milestone 0: Freeze Current Behavior

Goal:

- record the current orchestration behavior before moving it into runtime

Work:

- confirm current `main.cpp` responsibilities
- confirm current server startup rules
- confirm current source acceptance and sink attachment flow
- confirm current multiple-sinks-per-source behavior
- confirm current `--live` sink mode behavior
- document any assumptions that are still only implicit in code

Deliverables:

- update `README.md` if any behavior is still undocumented
- verify current integration tests still pass

Why this milestone exists:

- runtime extraction is safer when the current behavior is explicit and test-backed

## Milestone 1: Introduce Runtime Skeleton

Goal:

- add a `Runtime` abstraction without changing behavior yet

Work:

- add `core/runtime/runtime.h`
- add `core/runtime/runtime.cpp`
- add `RuntimeConfig`
- add a minimal `Runtime` public API:
  - `Start()`
  - `Close()`
- constructor receives process-level config and prebuilt server instances
- keep the orchestration logic functionally the same as current `main.cpp`

Deliverables:

- `Runtime` class compiles
- `main.cpp` becomes a thin bootstrap that constructs and runs `Runtime`

Tests:

- existing RTMP/WHIP integration tests must still pass unchanged

Why this milestone exists:

- it moves ownership boundaries first, before changing control plane behavior

## Milestone 2: Move Server Orchestration Into Runtime

Goal:

- move server thread management and source watcher management fully into runtime

Work:

- move server startup loops from `main.cpp` into `Runtime::Start()`
- move `NextSource()` watcher threads into runtime-owned threads
- move source registration into runtime-owned state
- add runtime-owned active source map keyed by `source_id`
- preserve duplicate-source rejection behavior or define the replacement behavior explicitly

Deliverables:

- runtime owns:
  - server threads
  - source watcher threads
  - active source registration
- `main.cpp` no longer contains direct orchestration logic

Tests:

- integration tests still pass
- add a runtime-level test for active source registration if feasible

Why this milestone exists:

- runtime cannot reconcile manifests until it owns active source state

## Milestone 3: Add Runtime State Types

Goal:

- make desired state and active state explicit in code

Work:

- add `runtime_state.h`
- define desired-state structures:
  - source spec
  - sink spec
  - route mapping
- define active-state structures:
  - active source handle
  - active sink handle
  - active route handle
- define locking strategy for runtime state

Deliverables:

- runtime stores state in named structures instead of ad hoc local variables

Tests:

- unit tests for any pure state helpers

Why this milestone exists:

- reconcile logic is much easier to reason about when state is explicit

## Milestone 4: Add Manifest DTOs And Validation

Goal:

- formalize the desired graph shape before adding HTTP

Work:

- add `runtime_manifest.h`
- add `runtime_manifest.cpp`
- define manifest DTOs:
  - `RuntimeManifest`
  - `RuntimeSourceSpec`
  - `RuntimeSinkSpec`
- implement validation helpers for:
  - duplicate `source_id`
  - duplicate `sink_id`
  - empty fields
  - unsupported sink mode
  - sink references to non-existent active sources

Deliverables:

- manifest parsing-independent validation layer
- structured error type for invalid manifests

Tests:

- unit tests for validation rules
- unit tests for rejection of unknown sources

Why this milestone exists:

- validation should exist before the transport layer so it can be tested directly

## Milestone 5: Add Reconcile Engine Without HTTP

Goal:

- let runtime apply manifests through a direct C++ API first

Work:

- add `Runtime::ApplyManifest(const RuntimeManifest&)`
- implement full-replace atomic validation and reconcile
- create missing sinks
- create missing routes
- keep unchanged sinks and routes
- remove obsolete routes
- remove obsolete sinks
- keep desired-state maps in sync with the accepted manifest

Deliverables:

- runtime can be driven programmatically without HTTP

Tests:

- unit tests for:
  - create on new manifest
  - no-op on unchanged manifest
  - remove on omission
  - full rejection with no partial mutation
- integration tests for:
  - one source -> one sink
  - one source -> multiple sinks
  - one record + one live sink on one source

Why this milestone exists:

- reconcile bugs are easier to isolate before adding HTTP parsing and request lifecycle

## Milestone 6: Add Shared Upsert Handler For CLI And HTTP

Goal:

- ensure CLI bootstrap and HTTP control both use the same runtime graph-update path

Work:

- define one shared runtime entry point for desired graph updates
- keep manifest validation and reconcile behind that shared handler
- make CLI route/bootstrap code call the same handler instead of mutating runtime state through a separate path
- design the HTTP layer as a thin adapter that calls the same handler later
- keep process-level server config separate from graph update input

Deliverables:

- one authoritative graph-update path in runtime
- no split between CLI graph setup semantics and HTTP graph setup semantics other than transport

Tests:

- existing CLI-driven integration tests still pass
- add tests that exercise the shared handler directly where practical

Why this milestone exists:

- this prevents the control plane from fragmenting into one CLI path and one HTTP path

## Milestone 7: Add SinkHandle And Route Ownership

Goal:

- make runtime-owned sink and subscription lifecycle explicit

Work:

- add `sink_handle.h`
- add `sink_handle.cpp`
- wrap `HlsSinkRunner` in a runtime-owned handle
- define `RouteHandle` that owns:
  - `source_id`
  - `sink_id`
  - `SourceSubscriptionPtr`
  - sink runner or sink handle linkage
- ensure multiple sinks can subscribe to the same source independently

Deliverables:

- runtime owns sink/route shutdown through named handle types

Tests:

- unit or integration coverage for:
  - independent subscriptions per sink
  - removing one sink does not stop sibling sinks on the same source

Why this milestone exists:

- source/sink graph ownership should be explicit before HTTP starts mutating it

## Milestone 8: Add HTTP Runtime Server

Goal:

- expose runtime control through `upsert_manifest`

Work:

- add `runtime_http.h`
- add `runtime_http.cpp`
- choose or implement a small HTTP server layer
- expose `POST /upsert_manifest`
- parse request JSON into `RuntimeManifest`
- call `Runtime::ApplyManifest(...)`
- return structured success/error responses

Deliverables:

- runtime can be controlled through HTTP

Tests:

- unit tests for request parsing if parser wrapper exists
- integration tests for:
  - valid upsert
  - invalid upsert
  - rejection of unknown source

Why this milestone exists:

- HTTP should stay a thin adapter over already-tested reconcile logic

## Milestone 9: Move CLI To Runtime Bootstrap Only

Goal:

- make `main` a pure process bootstrap layer

Work:

- keep only process-level flags in `main`
- remove sink graph construction from CLI flags
- instantiate servers from config
- instantiate runtime HTTP server
- start runtime and block until shutdown

Deliverables:

- graph management no longer lives in CLI
- runtime becomes the single owner of pipeline topology

Tests:

- update startup/integration tests to target the runtime-managed process

Why this milestone exists:

- this completes the transition from CLI routing to runtime routing

## Milestone 10: Implement Graceful Runtime Shutdown

Goal:

- guarantee deterministic close order and no partial shutdown races

Work:

- implement runtime shutdown sequence exactly as defined in `runtime.md`
- stop HTTP first
- freeze reconcile
- close routes/subscriptions
- close sinks
- close servers
- join watcher threads
- clear state

Deliverables:

- `Runtime::Close()` becomes the authoritative lifecycle shutdown path

Tests:

- tests for repeated `Close()`
- tests for shutdown during active ingest
- tests for shutdown during or near manifest update if feasible

Why this milestone exists:

- runtime lifecycle is one of the highest-risk parts of this redesign

## Milestone 11: Add Observability For Runtime State

Goal:

- make runtime behavior debuggable without attaching a debugger

Work:

- add structured logs for:
  - source accepted
  - source removed
  - sink created
  - sink removed
  - route created
  - route removed
  - manifest accepted
  - manifest rejected
- optionally add an internal snapshot helper for tests

Deliverables:

- runtime actions are traceable in logs

Tests:

- verify behavior through state assertions; log assertions only if the project already tests logs elsewhere

Why this milestone exists:

- reconcile systems are much easier to debug when lifecycle events are visible

## Milestone 12: Hardening Pass

Goal:

- tighten correctness after the first full runtime path is working

Work:

- review locking and thread ownership
- review duplicate source behavior across RTMP and WHIP
- review sink replacement edge cases
- review full-replace failure atomicity
- review startup and shutdown races
- remove temporary compatibility paths no longer needed

Deliverables:

- cleaned-up runtime implementation
- updated docs for any implementation assumptions made during the work

Tests:

- run the full relevant test suite
- add regression tests for every bug discovered during the runtime rollout

Why this milestone exists:

- orchestration code often works before it is truly safe; this milestone is for making it robust

## Recommended Execution Order

The recommended order is:

1. Milestone 0
2. Milestone 1
3. Milestone 2
4. Milestone 3
5. Milestone 4
6. Milestone 5
7. Milestone 6
8. Milestone 7
9. Milestone 8
10. Milestone 9
11. Milestone 10
12. Milestone 11
13. Milestone 12

This order is deliberate:

- ownership first
- state second
- validation before transport
- reconcile before HTTP
- shutdown after the runtime shape is stable

## Definition Of Done For The Runtime Step

The runtime step is complete when:

- `main` only bootstraps process-level config and runtime startup
- runtime owns server orchestration
- runtime owns source registration
- runtime owns sink and route lifecycle
- `POST /upsert_manifest` is the only graph mutation API
- manifests are full-replace and applied atomically
- unknown sources cause full request rejection
- one source can feed multiple sinks
- graceful shutdown follows the agreed close order
- behavior is covered by unit and integration tests

## Explicit Deferrals

These should not be mixed into the milestones above unless we decide to expand scope:

- decoder abstraction
- encoder abstraction
- compose nodes
- transcoding
- warm swap graph changes
- partial manifest updates
- creating sources directly from the manifest
- WebRTC transcoding-specific runtime logic
