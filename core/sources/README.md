Media source abstractions and the default in-process buffered source implementation.

Design docs:

- `docs/source.md` — rings, timeline, subscriptions
- `docs/pipeline.md` — pull models, extended pull-based nodes
- `docs/execution_model.md` — threads, startup, buffer fill, close order

Current headers:

- `isource.h` — `ISource` interface and `SourceDescriptor`
- `isource_subscription.h` — packet subscription interface
- `source_packet.h` — source format and packet types
- `buffered_source.h` — thread-safe buffered `ISource` used by ingest servers
- `source_queue.h` — queue for handing accepted sources to the runtime

Concrete protocol-specific sources (RTMP, WHIP, ...) live under `core/servers/` and publish into `BufferedSource`.
