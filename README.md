# Vision AI Box - Deep Blue AI Lab

docker run --rm -it \
  --privileged \
  --network host \
  -v /home/ghost/Desktop/core3:/home/ghost/core3 \
  -v /dev:/dev \
  doxchanger/private:vision_ai_box_builder \
  bash

Inside the container, switch to the project user before building:

```sh
su - ghost
cd /home/ghost/core3
```

```
# The same command with explicit HTTP port publishing instead of host networking:
# docker run --rm -it \
#   --privileged \
#   -p 8080:8080 \
#   -v /home/ghost/Desktop/core3:/home/ghost/core3 \
#   -v /dev:/dev \
#   doxchanger/private:vision_ai_box_builder \
#   bash
```

Build and run USB mode:

```sh
cmake -S . -B build-usb -DCMAKE_BUILD_TYPE=Release -DCAMERA=USB
cmake --build build-usb -j"$(nproc)"
./build-usb/bin/vision_ai_box
```


Build and run RealSense mode:

```sh
cmake -S . -B build-realsense -DCMAKE_BUILD_TYPE=Release \
  -DCAMERA=REALSENSE -DLIBREALSENSE_DIR=/home/ghost/librealsense
cmake --build build-realsense -j"$(nproc)"
./build-realsense/bin/vision_ai_box
```

```
export VISION_AI_BOX_USERNAME=admin
export VISION_AI_BOX_PASSWORD='admin'
export VISION_AI_BOX_PORT=8080
```

The embedded web server listens on port `8080` by default. Set
`VISION_AI_BOX_PORT` to select another port. Credentials default to
`admin` / `change-me`; configure them with `VISION_AI_BOX_USERNAME` and
`VISION_AI_BOX_PASSWORD` before starting the application.

Open `http://<device-ip>:8080/` from a device on the same network. The login
creates an HttpOnly session cookie and redirects to `/dashboard`. Protected
endpoints include `/api/camera/status`, `/api/camera/start`,
`/api/camera/stop`, `/api/camera/stream`, and `/api/plugins`.

Plugin libraries are discovered at application startup but are not loaded. The
Plugin page lists available libraries with no active selection. Selecting a
plugin calls `POST /api/plugins`, which loads that library at runtime and makes
it the active plugin. Only the selected plugin receives frames.

## Implementation

### Worker Threads

The project uses several background threads to decouple camera capture, web I/O,
and recording work from the main application loop.

1. Main service loop
   - `main()` creates the camera object and starts the web server.
   - It then initializes the camera once, starts the monitor thread, and enters
     a `while (toggles.running)` loop.
   - This loop only waits and listens for shutdown signals; it does not perform
     heavy processing.

2. Camera reconnect monitor thread
   - `camera_reconnect_loop()` runs as a background thread.
   - It checks `ServiceToggles::camera_enabled`, `camera.check_device_state()`,
     and `camera.is_running()` in a loop.
   - If the camera is disabled, it stops the device and clears processing flags.
   - If the camera is unplugged while running, it stops capture and sets
     `camera_error = true`.
   - If the device becomes available again, it calls `camera.start()` and
     re-enables processing.

3. Web server accept thread
   - `WebServer::start()` creates a listening socket and starts `accept_loop()`
     in a dedicated thread.
   - `accept_loop()` repeatedly calls `accept()`, then spawns a new thread for
     each connected client via `handle_client()`.

4. Client request worker threads
   - Each client connection is handled by `WebServer::handle_client()` in its
     own thread.
   - The handler parses the HTTP request, authenticates the session, and
     dispatches to the camera, recording, and plugin endpoints.

5. USB camera acquisition thread
   - `UsbCamera::start()` starts `acquisition_loop()` in a worker thread.
   - The loop waits until `processing_enabled_` is true, reads frames from OpenCV
     `VideoCapture`, and publishes each frame to callbacks and the latest-frame
     buffer.
   - If the device becomes unreadable, it sets `running_ = false` and releases
     the capture device.

6. RealSense acquisition thread
   - `RealSenseCamera::start()` starts `acquisition_loop()` in a worker thread.
   - It polls frames with `pipeline_.try_wait_for_frames()` and optionally aligns
     depth frames to color.
   - Valid color frames are cloned and then distributed to callback listeners
     and the latest-frame cache.
   - The loop handles hot-unplug exceptions by logging a warning and continuing
     to monitor the device.

7. Recording callback thread flow
   - `BaseSystem` registers a frame callback with the selected camera.
   - When `start_recording()` is called, it enables recording and prepares the
     output writer.
   - Every received frame is checked for duplication and then written to the
     video file via `VideoWriter`.
   - `stop_recording()` closes the writer and disables recording.
   - `restart_recording()` stops then starts a new session.
   - `capture_frame()` writes a JPEG snapshot using an asynchronous worker.

8. Shutdown thread cleanup
   - On shutdown, the signal handler sets the toggles to false.
   - The main thread stops the web server, disables processing, stops the camera,
     and joins the monitor thread.
   - This ensures clean teardown without leaving background loops alive.

This design keeps the camera, HTTP server, and recording pipeline independent
while using shared atomic flags to coordinate state changes safely across threads.

### How Frames are managed for both Recording Pipeline & Web App Pipeline

The frame flow starts from the active camera backend:

- `UsbCamera::acquisition_loop()` reads raw frames with `capture_.read(frame)`.
- `RealSenseCamera::acquisition_loop()` waits for frames via
  `pipeline_.try_wait_for_frames()`, and optionally aligns depth to color before
  producing the RGB image.
- Both camera implementations build a `FrameContext` object and publish it
  through registered callbacks.

The actual flow is:

1. Camera backend produces a raw frame.
2. The frame is wrapped in `FrameContext` with a sequence number.
3. The camera stores the newest frame in `latest_frame_` and exposes it through
   `latest_frame()`.
4. Registered callbacks receive the same frame object.
5. `BaseSystem` subscribes to the camera callback for the recording pipeline.
6. `WebServer` subscribes to the same callback to maintain its latest stream
   frame.

For the recording pipeline:

- `BaseSystem::on_frame_received()` checks whether recording is active and
  whether the frame is valid.
- It ignores duplicate sequences using `last_written_sequence_`.
- If the writer is not opened yet, it calls `open_writer_if_needed()` and
  `try_open_writer()`.
- The frame is passed to a bounded recording queue; a recording worker then
  calls `writer_.write(*frame.color)` away from the camera acquisition thread.
- No resizing, downsampling, or frame averaging is performed before recording.
- The writer uses the camera frame size and a fixed FPS value of `30.0`.

For the web app pipeline:

- `WebServer` registers a callback that stores the latest color frame in
  `latest_stream_frame_` in a sequence-aware, condition-variable mailbox.
- Each HTTP client waits for a newer frame sequence and drops stale frames
  instead of building a backlog or transmitting the same frame repeatedly.
- The stream resizes only its web copy while preserving the source aspect ratio,
  then encodes it as JPEG and sends it as multipart MJPEG.
- Each client adapts through quality, resolution, and FPS profiles based on
  measured socket-send time and encoded byte size. Poor throughput lowers JPEG
  quality first, then resolution/FPS; recovery is slower to prevent oscillation.
- Per-client frame, skip, byte, encode, and send metrics are logged when the
  stream connection ends.

So the short answer is:

- Recording path: frames remain raw in memory, then `VideoWriter` applies the
  selected video codec when writing the recording. The default is `mp4v`; it can
  be overridden with `VISION_AI_BOX_FOURCC` or a GStreamer pipeline.
- Web stream path: frames remain raw in memory and are resized/ compressed to
  JPEG immediately before transmission as a multipart MJPEG stream.
- Captured stills use `cv::imwrite` with a `.jpeg` extension and are JPEG-
  compressed when saved.
- Both pipelines share the same camera frame source. RealSense depth frames
  remain `CV_16UC1` matrices and are not currently sent to either output.

The web stream adaptation does not change `CameraSettings`: camera capture
resolution and FPS remain shared by recording and AI processing. Plugin and
recording callbacks now enqueue into bounded drop-oldest worker queues, so
camera acquisition does not perform plugin inference or video writing inline.
JPEG bytes are cached by frame sequence and stream profile, allowing clients
with matching profiles to reuse an encoded frame.
