# Optimization Plan v1

This plan turns the ideas in `Optimization_v1.md` into a step-by-step execution roadmap for the current project before adding future AI pipeline consumers.

## Goal

Prepare the current embedded camera + web + recording system for AI integration by reducing frame duplication, simplifying state management, reducing CPU pressure, and making future frame consumers easier to add without destabilizing the system.

---

## Phase 1: Stabilize state management and shutdown flow

### Step 1.1 — Define a single explicit service state enum

Action:
- Add a single `SystemState` enum in the project core layer.
- Allowed states: `BOOT`, `CAMERA_INIT`, `CAMERA_READY`, `STREAMING`, `RECORDING`, `AI_PROCESSING`, `ERROR_RECOVER`, `SHUTDOWN`.
- Replace scattered `running`, `camera_enabled`, `processing_enabled`, and `camera_error` checks with a single state variable managed from a central coordinator.

Why:
- Current logic is split across main loop, reconnect loop, web controls, and camera lifecycle.
- Embedded reliability improves when one state owner decides which transitions are legal.

Implementation notes:
- keep atomic access for cross-thread reads
- expose state transition helpers like `request_start_camera()`, `request_stop_camera()`, `request_recording_start()`, `request_shutdown()`
- prevent invalid transitions from multiple threads

Acceptance:
- no thread is directly mutating camera lifecycle without going through the state manager
- invalid transitions are rejected or logged

---

### Step 1.2 — Centralize camera lifecycle commands

Action:
- Move camera start/stop logic into a single wrapper API instead of calling `camera.start()` directly in main and web server.
- Create a resource owner or service controller class responsible for:
  - device validation
  - start requests
  - stop requests
  - error recover flows

Why:
- There are multiple places that currently start or stop the camera.
- Future AI consumers will require consistent startup and shutdown behavior.

Acceptance:
- one function owns camera lifecycle transitions
- all start/stop commands follow the same state machine rules

---

### Step 1.3 — Add shutdown-safe state transitions

Action:
- Ensure `SIGINT` / `SIGTERM` always transitions to `SHUTDOWN` before stopping the web server and camera.
- Make shutdown idempotent and log each shutdown stage.

Why:
- Current shutdown logic works, but it is spread across several calls and flags.
- Embedded systems benefit from deterministic teardown and no lingering worker threads.

Acceptance:
- calling shutdown twice does not crash or deadlock
- all threads finish cleanly

---

## Phase 2: Reduce frame duplication and unnecessary copies

### Step 2.1 — Introduce a shared frame buffer abstraction

Action:
- Create a `FrameBuffer` or `FrameRingBuffer` component.
- Add methods like:
  - `push(const FrameContext&)`
  - `latest()`
  - `pop()`
  - `clear()`
- Keep only the newest valid frame for UI and lightweight consumers.

Why:
- Current code stores copies in multiple places, including `latest_stream_frame_`, `latest_frame_`, and recording writer writes.
- AI integration will require a cleaner and cheaper frame-sharing pattern.

Acceptance:
- camera thread writes to the buffer
- UI and recording read from the buffer instead of direct camera internals

---

### Step 2.2 — Separate capture path from consumer path

Action:
- Add a producer thread for camera capture only.
- Move all downstream consumers to independent reading paths:
  - recording consumer
  - web stream consumer
  - AI consumer (future)
  - diagnostics consumer

Why:
- A shared camera producer avoids multiple consumers contending for the same frame lifecycle.
- This is the correct structure before AI pipeline integration.

Acceptance:
- camera producer owns frame acquisition
- consumers are independent and have their own dropping rules

---

### Step 2.3 — Add drop-old-frame policy

Action:
- Define consumer-specific policies:
  - web UI: keep newest only
  - recording: fixed-rate, drop stale frames when needed
  - AI: skip old frames if behind schedule
  - diagnostics: best effort only

Why:
- Embedded hardware cannot process every frame for every consumer at once.
- The current design does not formally define what should be dropped first.

Acceptance:
- queue depth is bounded
- stale frames are discarded instead of accumulating

---

### Step 2.4 — Remove redundant frame copies in the web pipeline

Action:
- Replace direct sharing of camera frame data with a dedicated latest-web-frame snapshot.
- Keep only one latest frame for streaming and avoid storing multiple copies in unrelated members.

Why:
- The current web stream keeps a cached latest frame and re-encodes it in a loop.
- This is manageable now, but becomes wasteful when AI consumers and recording are added.

Acceptance:
- at most one active web snapshot exists at a time
- WebServer no longer holds unnecessary duplicate frame state

---

## Phase 3: Optimize camera acquisition and callback behavior

### Step 3.1 — Reduce camera polling churn

Action:
- Keep a single device watchdog or monitor owner.
- Remove repeated duplicated device-state checks across multiple layers.
- Add a sleep or wait mechanism with coarse polling only when needed.

Why:
- Polling every 250 ms in the main reconnect loop is acceptable, but repeated checks across systems create extra wakeups.

Acceptance:
- no redundant state check loops across multiple threads
- combined device health checks use one owner

---

### Step 3.2 — Make callback dispatch cheaper

Action:
- Store callbacks in a lightweight vector or a dedicated notifier object.
- Avoid heavy locking while dispatching if callback work is minimal.
- Prefer copy-on-notify patterns only when necessary.

Why:
- Current callback vectors are fine for a single UI and a single recorder, but future AI consumers will increase callback load.

Acceptance:
- callback dispatch remains bounded and low-latency
- no callback blocks the camera thread for long

---

### Step 3.3 — Introduce frame sampling for non-critical consumers

Action:
- Add a configurable sampling rate for recording and web UI.
- Example: record at 15 FPS while camera runs at 30 FPS, or stream at lower resolution/load when CPU high.

Why:
- Embedded systems often need to prioritize correctness over maximum frame rate.

Acceptance:
- consumer-specific sampling is configurable through runtime settings
- the camera thread is never blocked by downstream consumers

---

## Phase 4: Optimize web server for embedded use

### Step 4.1 — Cap maximum active client connections

Action:
- Add a max-client limit in `WebServer`.
- Reject or close extra clients when the limit is reached.

Why:
- One thread per client is expensive on a microcomputer.
- The application should not be vulnerable to client overload.

Acceptance:
- system remains stable even with many simultaneous browser clients

---

### Step 4.2 — Make streaming adaptive

Action:
- Add runtime adaptation for stream FPS and JPEG quality based on CPU load or active AI usage.
- Example: if AI is processing frames, reduce web stream quality automatically.

Why:
- Web streaming currently costs per-frame JPEG encode and can dominate CPU time.

Acceptance:
- stream quality falls back when CPU or frame queue is under pressure

---

### Step 4.3 — Minimize static and dynamic web payloads

Action:
- Avoid sending unnecessary JSON payloads for every poll.
- Cache camera status or stream state when not changed.

Why:
- Small embedded computers benefit from lower network and CPU overhead.

Acceptance:
- minimal status requests return quickly without heavy object serialization

---

## Phase 5: Improve recording pipeline for low-power systems

### Step 5.1 — Separate writer thread from camera callback thread

Action:
- Move `VideoWriter` operations out of the callback thread.
- Use a dedicated recording worker that consumes queued frames and writes them sequentially.

Why:
- Current `on_frame_received()` writes directly in the callback path.
- This creates more coupling to the capture thread and can affect latency under load.

Acceptance:
- camera capture is decoupled from file writes
- writer overhead does not block frame acquisition

---

### Step 5.2 — Make recording quality adaptive

Action:
- Add a runtime quality/fps profile for recording.
- For example: `QUALITY_BALANCED`, `QUALITY_LOW_POWER`, `QUALITY_HIGH_QUALITY`.

Why:
- Recording should not consume all CPU when AI or web UI is active.

Acceptance:
- recording can be degraded gracefully under load

---

### Step 5.3 — Add file-write backpressure protection

Action:
- If the disk or video writer is slow, drop frames instead of blocking the capture thread.

Why:
- Embedded systems can get stuck if the file writer lags behind the camera.

Acceptance:
- capture loop continues even if recording falls behind

---

## Phase 6: Prepare for AI pipeline integration

### Step 6.1 — Add a dedicated AI frame subscription path

Action:
- Create a `FrameSubscriber` abstraction with queue and callback semantics.
- AI consumers register themselves as a low-priority subscriber.

Why:
- Future AI features should not reuse camera callbacks directly without a controlled queue.

Acceptance:
- AI consumer subscribes via a defined interface, not by directly using camera internals

---

### Step 6.2 — Add queue depth and latency monitoring

Action:
- Measure per-consumer queue size, frame age, and processing latency.
- Log or expose these stats via HTTP for debugging.

Why:
- AI integration will fail silently if queues grow without a system-level view of backlog.

Acceptance:
- each consumer exposes metrics: `queue_depth`, `dropped_frames`, `avg_latency_ms`

---

### Step 6.3 — Establish AI safety policy

Action:
- Define runtime policy for AI mode:
  - reduce web stream quality
  - reduce recording FPS
  - skip stale frames
  - queue only newest frame if AI is slower than capture

Why:
- The AI pipeline should be allowed to run at a controlled rate, not at the full sensor rate.

Acceptance:
- AI pipeline can degrade gracefully without stalling the main service

---

## Phase 7: Validation and verification

### Step 7.1 — Build performance baselines

Action:
- Measure with camera active, web stream active, and recording active.
- Record CPU load and frame drop rate before and after each optimization step.

Why:
- Embedded tuning must be evidence-driven.

Acceptance:
- each phase includes measurable comparison numbers

---

### Step 7.2 — Test recovery paths

Action:
- Validate camera unplug/reconnect flow
- Validate shutdown path
- Validate recording resume after reconnect
- Validate web stream recovery after overload

Why:
- Recovery is more important than absolute throughput on embedded devices.

Acceptance:
- failover and reconnect remain stable under repeated device loss

---

### Step 7.3 — Run future-AI compatibility checks

Action:
- Add a test setup with one AI consumer on a lower-rate queue
- Ensure AI does not block recording or web streaming permanently

Why:
- The solution must hold up once AI consumers are introduced.

Acceptance:
- AI consumer can be added without redesigning the whole architecture

---

## Recommended implementation order

1. Add explicit state machine
2. Centralize camera lifecycle
3. Add frame ring buffer and drop-old-frame policy
4. Adapt web stream quality and client limits
5. Move writer work out of callback path
6. Add AI consumer queue and metrics
7. Validate CPU, queue depth, and recovery behavior

---

## Expected final result

After these steps, the project should be ready for AI augmentation without destabilizing the current real-time camera, web UI, and recording operations. The architecture will be:

- stable and explicit in state
- bounded in memory usage
- adaptive under CPU pressure
- easier to extend with AI consumers
- safe for embedded microcomputer deployment
