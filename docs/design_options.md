# Pipeline Design Options

This note summarizes the main pipeline design styles that show up in media systems, stream processors, and concurrent runtimes. The goal is not to treat them as mutually exclusive camps. In practice, mature systems combine several of them.

## 1. Pull-Based Pipelines

### Core idea
Downstream asks upstream for the next unit of work when it is ready.

### Main functionality
- Natural demand-driven flow control
- Clear ownership of scheduling at the consumer side
- Easy to keep memory bounded because work is requested instead of pushed arbitrarily

### Advantages
- Backpressure is naturally expressed as "do not request more yet"
- Good fit for deterministic, synchronous loops
- Usually simpler to reason about ordering and memory growth

### Disadvantages
- Can underutilize parallelism if everything is driven by one consumer loop
- Less convenient when the source is inherently event-driven
- Random access or blocking pulls may complicate stage design

### Best fit
- File readers
- Iterators
- Demand-driven decoders
- Simple single-thread media loops

## 2. Push-Based Pipelines

### Core idea
Upstream produces data and pushes it into the next stage.

### Main functionality
- Producer-driven scheduling
- Natural fit for event sources, interrupts, callbacks, and live inputs
- Low latency when downstream can keep up

### Advantages
- Good for live/evented systems
- Source can emit immediately when data arrives
- Often easier to integrate with network or device callbacks

### Disadvantages
- Backpressure must be designed explicitly
- Easy to over-buffer or overrun downstream consumers
- Error handling and load shedding become more important

### Best fit
- Network ingress
- Device capture
- Event buses
- Callback-heavy runtimes

## 3. Push-Pull Hybrid Pipelines

### Core idea
One side pushes input into a stage, while the other side pulls output from it.

### Main functionality
- Decouples input acceptance from output availability
- Supports internal buffering, reordering, batching, and codec delay
- Makes backpressure explicit through "need more input" or "drain output first"

### Advantages
- Very practical for codecs and staged transforms
- Handles zero/one/many output items per input item
- Gives a clean contract for bounded buffering

### Disadvantages
- More stateful than pure push or pure pull
- Requires callers to obey the protocol carefully
- `EAGAIN`-style contracts are easy to misuse

### Best fit
- FFmpeg decoder and encoder APIs
- Parsers
- Stateful transforms
- Stages with delayed output

## 4. Queue-Based Staged Pipelines (SEDA-Style)

### Core idea
Split the system into stages connected by explicit queues, with each stage managing a specific kind of work.

### Main functionality
- Stage isolation
- Queue-based decoupling
- Independent scheduling and resource control per stage
- Load conditioning and adaptive shedding when overloaded

### Advantages
- Good modularity
- Easier multicore scaling than a single synchronous loop
- A slow stage is visible as queue growth instead of hidden coupling
- Lets you tune batching, thread pools, and admission control per stage

### Disadvantages
- Queue growth increases latency
- Requires explicit overload policy
- More moving parts: threads, queues, batching, shutdown, observability
- Can become complex if every tiny step becomes a stage

### Best fit
- High-concurrency services
- Multi-stage media backends
- Pipelines where decode, process, and output have different costs

## 5. Dataflow Graph Pipelines

### Core idea
Model the program as a graph of transforms over data collections or streams rather than a single linear chain.

### Main functionality
- Branching, merging, fan-out, fan-in
- Parallel execution over multiple workers
- Graph-level optimization by a runtime or runner
- Windowing, watermarking, and trigger logic for unbounded streams

### Advantages
- Handles complex topologies better than a plain stage chain
- Strong fit for batch + streaming unification
- Good for distributed execution and declarative optimization
- Naturally supports branching outputs and aggregation

### Disadvantages
- More abstract than hand-written imperative loops
- Debugging can be harder because the runtime owns more decisions
- Stateful/event-time behavior has a learning curve

### Best fit
- Analytics pipelines
- ETL
- Multi-output processing graphs
- Large-scale distributed processing

## 6. Actor / Message-Passing Pipelines

### Core idea
Each actor owns its own state and communicates only by asynchronous messages.

### Main functionality
- Encapsulated state
- Sequential processing per actor mailbox
- Concurrency across actors
- Hierarchical supervision and failure handling

### Advantages
- Avoids shared-state locking in the common case
- Strong isolation between components
- Good fit for fault containment and distributed systems
- Mailboxes make work boundaries explicit

### Disadvantages
- Message protocols add design overhead
- Debugging cross-actor flow can be harder than debugging direct calls
- Poorly designed mailboxes can still become unbounded buffers
- Serialization and copying costs can matter in distributed variants

### Best fit
- Distributed services
- Supervisable concurrent subsystems
- Systems where ownership boundaries matter more than raw local throughput

## 7. Reactive Streams

### Core idea
Asynchronous stream processing with an explicit backpressure protocol so receivers are not forced to buffer unbounded data.

### Main functionality
- Demand signaling across asynchronous boundaries
- Non-blocking backpressure
- Standardized publisher/subscriber style contracts
- Bounded queue expectations between stages

### Advantages
- Strong backpressure semantics
- Good fit for asynchronous pipelines with thread-pool boundaries
- Better than naive push pipelines when overload must stay controlled

### Disadvantages
- Higher protocol complexity than simple loops
- Can feel abstract for low-level media code
- Works best when the whole stack respects the same demand contract

### Best fit
- Async application pipelines
- Service-to-service streaming APIs
- Libraries that need interoperable stream contracts

## 8. Ring Buffer / Lock-Free Event Pipelines

### Core idea
Use fixed-size circular buffers and sequence coordination instead of general-purpose queues.

### Main functionality
- Very low latency inter-thread handoff
- Preallocated storage
- Predictable memory usage
- Lock-free or mostly lock-free coordination
- Explicit consumer dependency graphs

### Advantages
- Excellent for low-latency, high-throughput local pipelines
- Avoids repeated allocation
- Good cache behavior when implemented carefully
- Bounded capacity is explicit

### Disadvantages
- Harder to implement correctly than ordinary queues
- Overflow policy must be deliberate
- Best for in-process concurrency, not a general distributed model
- Busy-wait strategies can waste CPU under contention

### Best fit
- Trading systems
- Real-time telemetry
- Low-latency media/control planes
- Hot in-process handoff paths

## 9. Batch and Micro-Batch Pipelines

### Core idea
Process accumulated data in chunks rather than item-by-item continuously.

### Main functionality
- Batching for throughput
- Reduced per-record scheduling overhead
- Checkpointed and replayable progress
- In micro-batch systems, "streaming" is implemented as many small batch executions

### Advantages
- High throughput
- Simpler fault tolerance than true record-at-a-time streaming in many systems
- Easier aggregation-heavy processing
- Often simpler operational model than fully continuous streaming

### Disadvantages
- Adds latency by design
- Batch boundaries can make near-real-time behavior harder
- Not ideal for sub-frame or ultra-low-latency media workloads

### Best fit
- ETL
- Analytics
- Log processing
- Near-real-time systems where milliseconds-to-seconds latency is acceptable

## Comparison by Main Concern

### Lowest conceptual complexity
- Pull-based
- Push-based

### Best explicit backpressure story
- Pull-based
- Push-pull hybrid
- Reactive streams

### Best for multicore stage isolation
- Queue-based staged pipelines
- Actor/message-passing

### Best for very low latency in-process handoff
- Ring buffer / lock-free pipelines

### Best for branching and large-scale execution
- Dataflow graph pipelines

### Best for throughput-oriented analytics
- Batch and micro-batch pipelines

## What FFmpeg Looks Like

FFmpeg mostly behaves like a hybrid of:
- pull-based demuxing (`av_read_frame`)
- push/pull codec stages (`send_packet`/`receive_frame`, `send_frame`/`receive_packet`)
- bounded internal buffering and backpressure via return codes

That means FFmpeg is not usually a graph runtime with autonomous readers everywhere. The caller drives progress. Stages accept input, buffer internal state as needed, and then expose output when ready.

## What GStreamer Looks Like

GStreamer exposes both push and pull scheduling modes at the pipeline/pad level, which makes it a clearer example of an explicitly schedulable media graph. That is one reason it is often easier than FFmpeg for building graph-shaped media applications.

## Which Design Usually Fits Which Goal

### If correctness and simplicity matter most
- Pull-based
- Push-pull hybrid

### If throughput under concurrency matters most
- Queue-based staged pipelines
- Actor/message-passing
- Ring buffer pipelines for the hottest local paths

### If topology is complex
- Dataflow graph

### If overload control is critical
- Reactive streams
- SEDA-style staged pipelines
- Pull-based demand-driven loops

### If latency must be extremely low
- Ring buffer / lock-free
- Carefully designed push-pull hybrids

### If you are building a live video switcher
- Packet-switch only: cheapest compute, weakest decodability guarantees
- Decode/switch/encode: strongest correctness, highest cost
- Hybrid staged design: best when you need bounded queues, observability, and controlled overload

## Practical Recommendations for Media Systems

### Small single-process tool
Use a synchronous pull or push-pull hybrid design.

### Multi-input live switcher
Use staged queues with explicit latency bounds, keyframe-aware switching rules, and a drop policy.

### Ultra-low-latency local pipeline
Use fixed-size buffers or a ring buffer on the hot path, but keep the rest of the system simpler.

### Distributed processing system
Prefer actor or dataflow designs where failure handling and scaling are first-class concerns.

## Sources

- GStreamer scheduling and push/pull design:
  - https://gstreamer.freedesktop.org/documentation/additional/design/scheduling.html
  - https://gstreamer.freedesktop.org/documentation/additional/design/push-pull.html
- Reactive Streams:
  - https://www.reactive-streams.org/
- SEDA paper:
  - https://people.eecs.berkeley.edu/~brewer/papers/SEDA-sosp.pdf
- Apache Beam model:
  - https://beam.apache.org/documentation/programming-guide/
  - https://beam.apache.org/documentation/basics/
- Akka actor model:
  - https://doc.akka.io/libraries/akka-core/current/typed/guide/actors-intro.html
- LMAX Disruptor:
  - https://lmax-exchange.github.io/disruptor/user-guide/
- Spark Structured Streaming micro-batch model:
  - https://spark.apache.org/docs/3.5.8/structured-streaming-programming-guide.html


recorder_1 := NewOutput(record_url_1)
live_1 := NewOutput(live_url_1)

recorder_2 := NewOutput(record_url_2)
live_2 := NewOutput(live_url_2)

input_pipe_1 := NewPipe()
input_pipe_2 := NewPipe()

input_1 := NewInput(url_1)
input_2 := NewInput(url_2)

multi_cast_1 := NewMulticaster(input_1, []Stream{input_pipe_1.Input, recorder_1, live_1})
multi_cast_2 := NewMulticaster(input_2, []Stream{input_pipe_2.Input, recorder_2, live_2})

pipe_3 := NewPipe()
pipe_4 := NewPipe()

switch_1 := NewSwitch([]Stream{input_pipe_1.Output, input_pipe_2.Output}, pipe_3.Input, opts.WithTimeLine{
  "0.5" : in_1,
  "50.21" : in_2,
  "60.00" : in_1,
})

recorder_3 := NewOutput(record_url_3)
live_3 := NewOutput(live_url_3)


multicast_3 := NewMulticaster(pipe_3.Output, []Stream{pipe_4.Input, recorder_3, live_3})


-------> pipe_4.Output


recorder_4 := NewOutput(record_url_4)
live_4 := NewOutput(live_url_4)

recorder_5 := NewOutput(record_url_5)
live_5 := NewOutput(live_url_5)

input_pipe_5 := NewPipe()
input_pipe_6 := NewPipe()

input_3 := NewInput(url_3)
input_4 := NewInput(url_4)

multi_cast_3 := NewMulticaster(input_3, []Stream{input_pipe_5.Input, recorder_4, live_4})
multi_cast_4 := NewMulticaster(input_4, []Stream{input_pipe_6.Input, recorder_5, live_5})

pipe_7 := NewPipe()
pipe_8 := NewPipe()


switch_2 := NewSwitch([]Stream{input_pipe_5.Output, input_pipe_6.Output}, pipe_7.Input, opts.WithTimeLine{
  "0.5" : in_1,
  "50.21" : in_2,
  "60.00" : in_1,
})

recorder_6 := NewOutput(record_url_6)
live_6 := NewOutput(live_url_6)

multicast_5 := NewMulticaster(pipe_7.Output, []Stream{pipe_8.Input, recorder_6, live_6})

pipe_9 := NewPipe()
pipe_10 := NewPipe()


decoder_1 := NewDecoder(pipe_8.Output, pipe_9.Input)
decoder_2 := NewDecoder(pipe_4.Output, pipe_10.Input)

pipe_11 := NewPipe()
pipe_12 := NewPipe()
pipe_13 := NewPipe()

multicast_5 := NewMulticaster(pipe_9.Output, []Stream{pipe_11.Input, pipe_12.Input})

videoLayout := VideoLayout{
  {x1, y1}, {x2, y2}
}

pipe_14 := NewPipe()
pipe_15 := NewPipe()

scene_compose := NewSceneComposer([]Stream{pipe_11.Output, pipe_12.Output}, pipe_14.Input, opts.WithVideoLayout(videoLayout))
inference_mdoel := NewInferenceModel([]Stream{pipe_11.Output, pipe_12.Output}, pipe_15.Input, opts.modelType("StableDiffusion"))


---------------> pipe_14.Output
---------------> pipe_15.Output

pipe_16 := NewPipe()

switch_3 := NewSwitch([]Stream{pipe_14.Output, pipe_15.Output}, pipe_16.Input, opts.WithExplicitSwitch(), opts.WithSelectedInputId(0))

pipe_17 := NewPipe()


encoder := NewEncoder(pipe_16.Output, pipe_17.Input)


recorder_7 := NewOutput(record_url_7)
live_7 := NewOutput(live_url_7)

encoeded_multicast := NewMulticaster(pipe_17.Output, []Stream{recorder_7, live_7})

encoeded_multicast.Start()
live_7.Start()
recorder_7.Start()
encoder.Start()
switch_3.Start()
scene_compose.Start()
inference_mdoel.Start()
.... (other to start)

on each change on graph we get the diff and based on diff try to change the pipeline remove some of them and some new ones.



