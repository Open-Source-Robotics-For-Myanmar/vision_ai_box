# Optimized v1 Summary

This document records the implementation work completed for the first optimization pass based on the roadmap in `OptimizationPlan_v1.md`.

## Scope completed in this pass

The work focused on the highest-value embedded reliability and CPU-pressure items before adding future AI consumers:

1. Centralized state management
2. Reduced duplicate web-frame state
3. Added bounded client handling for the web server
4. Routed shutdown and camera stop paths through the service state manager

---

## 1) Centralized state management

Implemented in [include/Toggles.hpp](../include/Toggles.hpp)

Changes made:
- Added `SystemState` with explicit lifecycle states:
  - `BOOT`
  - `CAMERA_INIT`
  - `CAMERA_READY`
  - `STREAMING`
  - `RECORDING`
  - `AI_PROCESSING`
  - `ERROR_RECOVER`
  - `SHUTDOWN`
- Added transition validation and guards so invalid state changes are rejected.
- Added request helpers for:
  - `request_start_camera()`
  - `request_stop_camera()`
  - `request_recording_start()`
  - `request_shutdown()`

Why this matters:
- Previous logic distributed camera lifecycle decisions across the main loop and web control flow.
- The new state owner reduces invalid transitions and makes future AI consumers easier to add safely.

Related updates:
- [main.cpp](../main.cpp) now uses `request_shutdown()` and `request_stop_camera()` instead of directly mutating scattered flags.

---

## 2) Shutdown-safe state transitions

Implemented in [main.cpp](../main.cpp)

Changes made:
- Signal handler now requests a clean shutdown through the centralized state manager instead of directly clearing multiple booleans.
- Main loop waits on the service `running` flag while respecting the state lifecycle.
- Shutdown path clears the camera state before stopping the web server and stopping the camera.

Result:
- shutdown is now routed through a single decision path rather than several disconnected flag assignments.

---

## 3) Shared latest-frame buffer for web streaming

Implemented in [web_server/WebServer.hpp](../web_server/WebServer.hpp)

Changes made:
- Added a `FrameBuffer` abstraction to hold the newest valid camera frame.
- Added methods for:
  - `push(const FrameContext&)`
  - `latest()`
  - `pop()`
  - `clear()`
- Replaced the ad-hoc `latest_stream_frame_` mutex storage with a dedicated bounded frame holder.

Why this matters:
- This reduces duplicate, scattered frame state and gives the web stream a clear rule: keep only the newest valid frame.
- It matches the intended embed-friendly pattern before AI consumers are introduced.

---

## 4) Web server client protection and reduced overload risk

Implemented in [web_server/WebServer.cpp](../web_server/WebServer.cpp)

Changes made:
- Added a max client connection cap via `kMaxClientConnections = 8`.
- `accept_loop()` now rejects additional clients when the limit is reached and closes the socket immediately.
- The web stream read path uses the shared `FrameBuffer::latest()` call instead of reading direct local state.

Why this matters:
- Small embedded devices should not become unstable when too many browser clients connect.
- This is a practical step toward safer embedded operation and lower CPU pressure.

---

## 5) Cleaner frame reset flow for camera setting changes

Implemented in [web_server/WebServer.cpp](../web_server/WebServer.cpp)

Changes made:
- When a camera setting change restarts or resets the stream, the latest web snapshot is cleared via `latest_stream_frame_.clear()` instead of ad-hoc mutex resets.

Why this matters:
- It keeps the clear/reset behavior centralized and consistent with the shared frame bucket pattern.

---

## Verification

Build verification was performed in the project’s Docker builder environment using the README flow and the corrected repo path:

```sh
docker exec -it recursing_margulis bash -lc 'su - ghost -c "cd /home/ghost/core3 && rm -rf build build-usb build-realsense && cmake -S . -B build-usb -DCMAKE_BUILD_TYPE=Release -DCAMERA=USB && cmake --build build-usb -j$(nproc)"'
```

Result:
- CMake configuration succeeded
- OpenCV, nlohmann_json, Threads, and FFmpeg were detected
- Final output included:
  - `[100%] Linking CXX executable vision_ai_box`
  - `[100%] Built target vision_ai_box`

This confirms the optimization pass built successfully in the supported container environment.

---

## Remaining Roadmap Items (Phases 3 to 6)

The foundational state safety and web frame buffering (Phases 1 & 2) are complete. For the next pass before integrating AI consumers, focus on these remaining items from the plan:

- Decouple Video Recording Thread (Phase 5.1): Move OpenCV `VideoWriter` calls out of the camera callback thread into a dedicated worker thread to prevent disk I/O latency from dropping live frames.
- Adaptive Streaming & Quality Scaling (Phase 4.2 & 5.2): Dynamically lower JPEG encoding quality or frame rate when CPU usage spikes or when AI consumers become active.
- AI Subscription Queue (Phase 6.1): Create a dedicated, low-priority subscriber queue with latency monitoring so future AI models process frames at their own pace without affecting UI or recording feeds.

### Implementation direction for the next pass

1. Recording worker decoupling
   - Replace direct writer writes inside `BaseSystem::on_frame_received()` with a queue that is drained by a dedicated writer thread.
   - Keep the capture thread focused on acquisition and callbacks.
   - Buffer a bounded number of frames so the producer never blocks indefinitely when disk I/O becomes slow.

2. Adaptive stream quality
   - Introduce runtime FPS and JPEG quality tuning based on current CPU utilization and active consumer count.
   - When AI processing starts, reduce the web stream quality before lowering camera capture rate.
   - Preserve a single newest frame for UI display instead of trying to keep a backlog of historical frames.

3. AI-only subscriber queue
   - Add a frame subscription object for AI consumers that uses a low-priority queue and explicit dropping rules.
   - Track queue depth, oldest frame age, and frame drop count to expose a health signal for future AI jobs.
   - Allow AI consumers to subscribe at their own cadence instead of forcing every frame into the same callback path.

This next pass keeps the project aligned with the original optimization roadmap while preserving the already-completed state safety and buffering work.

---

## Status of the plan

This is the first optimization pass, not the entire future AI pipeline architecture. The implemented items cover the foundational parts of Phase 1 and Phase 2 from the plan:

- explicit service state management
- safer shutdown transition handling
- fewer scattered lifecycle flags
- shared frame buffer for web streaming
- bounded client handling

The remaining roadmap items such as recording worker decoupling, adaptive streaming policy, and producer/consumer frame scheduling remain good candidates for the next optimization pass.
