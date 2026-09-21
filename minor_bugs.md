**Current Architecture**

- CMake builds a C++20 executable with OpenCV, FFmpeg, `nlohmann_json`, threads, and `dl`.
- USB is the default camera backend; RealSense is selected with `-DCAMERA=REALSENSE`.
- `main()` initializes the camera, starts the web server, discovers plugins, and runs a reconnect-monitor thread.
- Camera frames are published through `FrameContext` callbacks to:
  - `PluginManager`
  - `WebServer` MJPEG streaming
  - `BaseSystem` recording
- The web server owns `BaseSystem`, so recording is operational through HTTP routes.

Relevant entry points: `main.cpp`, `CMakeLists.txt`, `WebServer.cpp`.

**Implemented Features**

- USB and RealSense camera abstractions.
- Camera start/stop and reconnect monitoring.
- Runtime camera settings.
- HTTP authentication with session cookies.
- MJPEG camera streaming.
- Recording to `media/recorded_videos`.
- Snapshot capture to `media/capture_photos`.
- Media discovery and file serving.
- Runtime plugin discovery, loading, selection, enabling, disabling, unloading, and settings updates.
- Session logging through the web UI.

**Plugin Status**

Only the face-recognition plugin is currently built:

- `CMakeLists.txt`
- `CMakeLists.txt`
- `FaceRecognitionPlugin.cpp`

However, face recognition is only a prototype. Its `process()` method does not perform recognition; it only checks whether a frame is valid. The heartbeat logs a message every three seconds.

The other plugins are not active. Some CMake files are empty, and the depth-estimation CMake file references source/target names that do not appear to exist.

**Important Gaps and Risks**

1. **System state is mostly disconnected.**  
   `ServiceToggles::SystemState` defines a state machine, but startup and reconnect logic directly modify atomics without consistently calling `request_transition()`. The application can remain in `BOOT` while the camera is running.

2. **Plugin selection is runtime-only.**  
   Plugins are discovered but not loaded on startup. No plugin receives frames until the web UI selects one through `POST /api/plugins`.

3. **Detached snapshot worker can outlive `BaseSystem`.**  
   `capture_frame()` launches a detached thread capturing `this`. During shutdown, that thread may access `logger_` after `BaseSystem` has been destroyed.

4. **HTTP parsing is fragile.**  
   Malformed request lines or invalid `Content-Length` values can cause uncaught exceptions in client threads.

5. **Media path validation is insufficiently strict.**  
   The prefix check can accept paths such as `media_evil` because it checks string prefixes rather than path-component boundaries.

6. **Authentication is suitable only for a trusted network.**  
   Credentials are compared directly, sessions never expire, and cookies are sent over plain HTTP without `Secure`.

7. **Camera settings are not validated.**  
   Arbitrary resolutions, FPS values, and device indexes can be submitted to the camera backend.

8. **Documentation is partly stale.**  
   The README describes `BaseSystem` as being registered directly by `main()`, but it is actually constructed inside `WebServer`.

**Verification**

A build/configuration check could not be completed because the environment lacks both `cmake` and `rg`. The existing worktree also contains unrelated user changes: deleted `plugins/vision_algorithms/*` files and an untracked `example` directory.