# argus Redesign

## Purpose

This note captures the current redesign direction for `argus`.

The immediate goals are:

- move the sink implementation to C++
- make C++ the main implementation stack of `argus`
- introduce explicit abstractions for the main runtime components
- keep protocol-specific and library-specific details behind our own interfaces

## Context

`argus` started as a narrow ingest engine with:

- RTMP publish ingest
- WebRTC WHIP publish ingest
- HLS output

The server/session layer is already moving to C++.
The HLS sink is the main remaining C-heavy runtime piece and should move next.

## Main Direction

The redesign should follow these principles:

- C++ is the primary implementation language for `argus`
- C ABI should remain only where interoperability is still useful
- runtime components should depend on our abstractions, not directly on transport or library details
- packet/frame/timing ownership should be explicit at each boundary
- queues and buffering should live behind typed ports, not be invented ad hoc per component

## First Migration Step

The first concrete step is:

1. move `core/sinks` to C++
2. preserve current behavior during the move
3. keep tests green
4. only then add the broader abstractions around the migrated code

This is intentionally incremental.

## Planned Abstractions

The current target abstraction set is:

- `Server`
- `Source`
- `Sink`
- `Decoder`
- `Encoder`
- `Composer`

These should be implemented as our own interfaces, not as wrappers that leak:

- socket details
- libdatachannel types
- FFmpeg types
- protocol-specific concepts at the wrong layer

## Component Model

There are two kinds of components in this design.

### Autonomous Runtime Components

These own their own runtime behavior and lifecycle:

- `Server`
- `Source`
- `Sink`

### Processing Components

These are processing nodes in the media path:

- `Decoder`
- `Encoder`
- `Composer`

`Decoder` and `Composer` are **extended pull-based** (no background thread, no
`Start()` — `Close()` only for now). See `docs/pipeline.md`.

`Encoder` is queue-based with a **runner thread**, raw inputs, encoded output,
and subscriptions for multiple sinks.

This distinction matters because autonomous components manage sessions, state,
buffers, and shutdown, while extended pull-based components fill buffers on
`ReadPacket` when downstream demands data.
## Server

`Server` is responsible for:

- accepting or rejecting inbound connections
- owning transport/session lifecycle
- creating `Source` objects from accepted connections
- shutting down itself and all owned sessions when closed

`Server` should not be modeled as a static map of predefined sources.

That means a design like:

```cpp
GetSource(source_id) -> Source*
```

is not a good primary contract for ingest servers.

The real flow is:

- a connection arrives
- the server validates it
- the server accepts or rejects it
- if accepted, the server creates a `Source`

So the better mental model is:

- `Server` manages connections
- accepted connections become `Source` abstractions

### Candidate Shape

```cpp
struct IServer {
  virtual ~IServer() = default;
  virtual void Start() = 0;
  virtual void Close() = 0;
};
```

Accepted sources should be surfaced through:

- a callback
- an event sink
- a registry
- or a poll API

For example:

```cpp
struct IServerListener {
  virtual ~IServerListener() = default;
  virtual void OnSourceAccepted(std::shared_ptr<ISource> source) = 0;
  virtual void OnSourceRejected() = 0;
};
```

The exact surfacing mechanism is still open, but the ownership direction is not.

## Source

`Source` is the abstraction over one accepted publisher/session.

It is created by the server after connection acceptance and owns:

- source/session-local buffering
- packet readiness
- session-local state
- source-local shutdown

### Read Direction

The preferred direction is pull-based:

- downstream asks the source for packets
- source reads from the underlying connection only as needed, unless it internally must buffer

This gives:

- clearer backpressure
- simpler ownership
- less accidental buffering

### Candidate Shape

```cpp
struct ISource {
  virtual ~ISource() = default;
  virtual SourceId Id() const = 0;
  virtual Status ReadPacket(Packet* out) = 0;
  virtual void Close() = 0;
};
```

### Source Lifecycle

`Source` does have lifecycle internally.

Examples:

- initializing
- live
- drained
- failed
- closed

But it does not need a large public lifecycle API unless we later prove we need one.

So the design answer is:

- do not ignore lifecycle
- do not overexpose lifecycle
- treat `Source` as a first-class runtime object, not just a thin connection wrapper

## Sink

`Sink` is the first abstraction that should move fully into C++.

It should own:

- output lifecycle
- mux/write state
- output-local timing policy
- shutdown/flush behavior

It should not rely on callers to reinvent timing or queue policy ad hoc.

The current HLS sink migration should be the template for later output abstractions.

### Candidate Shape

```cpp
struct ISink {
  virtual ~ISink() = default;
  virtual Status Start() = 0;
  virtual Status WritePacket(const Packet& packet) = 0;
  virtual void Close() = 0;
};
```

## Decoder

`Decoder` is an **extended pull-based** processing node.

It should:

- consume encoded packets from source via `ReadPacket`
- emit raw packets via `ReadPacket`
- hide backend-specific decode details
- maintain an internal packet-count buffer filled from upstream on demand
- expose **no `Start()`** lifecycle for now — only `Close()`
- run **without a background thread**

Decoder fan-out (multiple readers) is deferred. See `docs/pipeline.md`.

## Encoder

`Encoder` is a **queue-based autonomous component with a runner thread**.

It should:

- pull **raw** input only (video frames, text, location, audio PCM, any
  non-encoded `media_type`) from one or more composer/upstream branches
- synchronize inputs that arrive at different rates
- encode / remux into encoded output packets
- enforce output cadence and monotonic output timestamps when configured
- expose **subscriptions** so multiple sinks can `ReadPacket` encoded output
- own `Start()` / `Close()` lifecycle

Sinks on the encode branch only read encoded packets from encoder subscriptions;
they do not encode. See `docs/pipeline.md`.

## Composer

`Composer` is an **extended pull-based** raw-domain node.

It should:

- pull raw packets from decoder (via `ReadPacket`) and eventually multiple inputs
- emit raw packets via `ReadPacket`
- own multi-input composition and raw-domain synchronization **inside `ReadPacket`**
- maintain an internal packet-count buffer with the same fill policy as decoder
- expose **no `Start()`** lifecycle for now — only `Close()`
- run **without a background thread**

It should not leak renderer or backend specifics into the control layer.

See `docs/pipeline.md` and `docs/result.md`.
## Typed Ports

Queues, buffering, and connectivity should be represented through typed ports.

This is a central part of the redesign.

Instead of every component inventing custom queue semantics, components should expose:

- input ports
- output ports
- media type carried by each port

### Why

This makes:

- graph validation clearer
- queue ownership explicit
- component connections type-safe
- later graph/runtime abstractions easier to implement

### Candidate Port Model

```cpp
enum class PortKind {
  EncodedVideo,
  EncodedAudio,
  RawVideo,
  RawAudio,
  Metadata,
};

struct InputPort {
  PortId id;
  PortKind kind;
};

struct OutputPort {
  PortId id;
  PortKind kind;
};
```

The buffering contract should belong to the port boundary, not be duplicated across unrelated components.

## Packet And Frame Models

These abstractions should use our shared media models.

The target is to align them with the shared packet/frame models already present in the project history and direction, rather than inventing backend-native models at each boundary.

That means:

- `Packet` should remain our transport between packet-domain components
- `Frame` should remain our transport between raw-domain components

## Ownership Summary

### Server Owns

- connection acceptance/rejection
- session lifecycle
- creation of `Source` instances

### Source Owns

- source-local buffer
- packet readiness
- connection-backed source state

### Sink Owns

- output-local write state
- output-local timing/muxing policy
- flush/close behavior

### Ports Own

- type boundary
- queue/buffer contract between components

## Recommended Implementation Order

1. move `Sink` to C++
2. define shared C++ models for packet/frame/status/ids
3. define `ISink`
4. define `ISource`
5. define `IServer`
6. move current RTMP/WebRTC code onto `Server` -> `Source` boundaries
7. then define `Decoder`, `Encoder`, and `Composer`
8. finally introduce explicit typed port connections between processing components

## Open Questions

These points are still open and should be resolved before the full abstraction layer is implemented:

- how accepted `Source` objects are surfaced from `Server`
- whether `Source` should support only blocking pull or also a readiness/poll contract
- whether ports should own internal queues directly or expose a queue policy object
- how much of the current packet/frame model should be reused unchanged versus reshaped for C++

## Current Conclusion

The redesign direction is:

- first move sinks to C++
- make C++ the main stack of `argus`
- treat `Server`, `Source`, `Encoder`, and `Sink` as autonomous runtime abstractions
- treat `Decoder` and `Composer` as **extended pull-based** nodes (`ReadPacket`, fill-on-read, no thread)
- connect components through typed ports with explicit buffer ownership
- see `docs/pipeline.md` for threading, buffer fill policy, and encode branch layout
- see `docs/execution_model.md` for startup, update, and shutdown ordering

This is the current working plan for the next phase of `argus`.