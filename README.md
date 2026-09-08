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
./build-usb/vision_ai_box
```


Build and run RealSense mode:

```sh
cmake -S . -B build-realsense -DCMAKE_BUILD_TYPE=Release \
  -DCAMERA=REALSENSE -DLIBREALSENSE_DIR=/home/ghost/librealsense
cmake --build build-realsense -j"$(nproc)"
./build-realsense/vision_ai_box
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
`/api/camera/stop`, and `/api/camera/stream`.

## Implementation Diagrams

### 1. Overview State Workflow

```mermaid
stateDiagram-v2
  [*] --> Starting
  Starting --> CameraStarting: camera.start()
  CameraStarting --> StartupFailed: initialization fails
  CameraStarting --> CameraRunning: camera started
  CameraRunning --> WebServerStarting: WebServer.start(port)
  WebServerStarting --> StartupFailed: socket/bind/listen fails
  WebServerStarting --> Running: server listening
  Running --> Running: HTTP request handled
  Running --> Stopping: SIGINT or SIGTERM
  StartupFailed --> Stopping: cleanup
  Stopping --> WebServerStopped: web_server.stop()
  WebServerStopped --> CameraStopped: camera processing disabled and camera.stop()
  CameraStopped --> [*]

  state Running {
    [*] --> WebServerReady
    WebServerReady --> CameraStreamAvailable: authenticated stream request
    CameraStreamAvailable --> WebServerReady: client disconnects or stream stops
    WebServerReady --> CameraControl: authenticated start/stop request
    CameraControl --> WebServerReady
  }
```

### 2. Detailed State Workflow

```mermaid
stateDiagram-v2
  [*] --> MainReady
  MainReady: Create ServiceToggles, Logger, and selected camera
  MainReady --> CameraConfiguring: camera.start()

  state CameraConfiguring {
    [*] --> StreamsConfigured
    StreamsConfigured: Configure color, optional depth, and alignment
    StreamsConfigured --> PipelineStarted
    PipelineStarted: pipeline.start(config) or VideoCapture.open()
    PipelineStarted --> SensorsConfigured: Camera opened
    SensorsConfigured: Configure auto exposure and auto white balance
    SensorsConfigured --> WorkerStarted
    PipelineStarted --> CameraInitFailed: RealSense error, standard exception, or open failure
  }

  CameraConfiguring --> CameraRunning: Worker thread started
  CameraInitFailed --> StartupFailed
  MainReady --> StartupFailed: camera.start() returns false

  state CameraRunning {
    [*] --> WaitingForFrames
    WaitingForFrames --> WaitingForFrames: poll or read next frame
    WaitingForFrames --> FramePublished: clone frame into FrameContext
    FramePublished: LatestFrameBuffer.publish(frame)
    FramePublished --> WaitingForFrames
    WaitingForFrames --> ProcessingPaused: processing disabled
    ProcessingPaused --> WaitingForFrames: processing enabled
  }

  CameraRunning --> WebStarting: WebServer.start(port)
  state WebStarting {
    [*] --> SocketCreated
    SocketCreated --> Listening: bind() and listen() succeed
    SocketCreated --> WebStartFailed: socket, bind, or listen fails
  }

  WebStarting --> ServingHTTP: server thread started
  WebStartFailed --> Shutdown

  state ServingHTTP {
    [*] --> AwaitingRequest
    AwaitingRequest --> LoginRequest: POST /api/login
    LoginRequest --> Authenticated: valid credentials; set session cookie
    LoginRequest --> AwaitingRequest: invalid credentials; return 401
    Authenticated --> DashboardRequest: GET /dashboard
    Authenticated --> StatusRequest: GET /api/camera/status
    Authenticated --> CameraStartRequest: POST /api/camera/start
    Authenticated --> CameraStopRequest: POST /api/camera/stop
    Authenticated --> StreamRequest: GET /api/camera/stream
    DashboardRequest --> AwaitingRequest: return HTML
    StatusRequest --> AwaitingRequest: return JSON status
    CameraStartRequest --> AwaitingRequest: start camera and return status
    CameraStopRequest --> AwaitingRequest: stop camera and return status
    StreamRequest --> ReadLatestFrame
    ReadLatestFrame: camera.latest_frame(frame)
    ReadLatestFrame --> EncodeJPEG: frame available
    ReadLatestFrame --> ReadLatestFrame: no frame; wait 25 ms
    EncodeJPEG: JPEG encoding
    EncodeJPEG --> SendMJPEG: send multipart JPEG
    SendMJPEG --> ReadLatestFrame: stream generation unchanged
    SendMJPEG --> AwaitingRequest: client disconnects or stream stops
  }

  CameraStartRequest --> CameraRunning: camera.start() succeeds
  CameraStopRequest --> CameraStopped: camera.stop()
  CameraStopped --> CameraRunning: camera.start() succeeds
  ServingHTTP --> Shutdown: SIGINT, SIGTERM, or WebServer.stop()

  StartupFailed --> Shutdown
  Shutdown: Stop server, disable processing, stop camera, join threads
  Shutdown --> [*]
```

### 3. Function Call Workflow

```mermaid
sequenceDiagram
  participant OS as OS signal
  participant Main as main()
  participant Camera as SelectedCamera
  participant Pipeline as USB/OpenCV or RealSense pipeline
  participant Web as WebServer
  participant Client as Browser
  participant Buffer as LatestFrameBuffer

  Main->>Camera: start()
  Camera->>Camera: initialize()
  alt RealSense backend
    Camera->>Pipeline: configure streams
    Camera->>Pipeline: pipeline.start(config)
    Pipeline-->>Camera: pipeline profile
    Camera->>Pipeline: configure sensor defaults
  else USB backend
    Camera->>Pipeline: open VideoCapture
  end
  Camera->>Camera: start_worker()
  Camera-->>Main: true

  Main->>Web: start(web_port)
  Web->>Web: socket(), bind(), listen()
  Web-->>Main: true

  loop camera worker
    Camera->>Pipeline: read or try_wait_for_frames()
    Pipeline-->>Camera: frame data
    Camera->>Camera: copy frame into FrameContext
    Camera->>Buffer: publish(frame)
  end

  Client->>Web: POST /api/login
  Web-->>Client: session cookie
  Client->>Web: GET /api/camera/stream
  Web->>Web: authenticate request
  loop while stream is active
    Web->>Buffer: camera.latest_frame(frame)
    Buffer-->>Web: latest FrameContext
    Web->>Web: encode frame as JPEG
    Web-->>Client: multipart JPEG frame
  end

  Client->>Web: POST /api/camera/stop
  Web->>Camera: set_processing_enabled(false)
  Web->>Camera: stop()
  Camera->>Camera: running = false
  Camera->>Camera: join worker
  Camera->>Pipeline: stop or release capture
  Web-->>Client: {running:false}

  OS->>Main: SIGINT or SIGTERM
  Main->>Web: stop()
  Web->>Camera: stop_camera()
  Main->>Camera: set_processing_enabled(false)
  Main->>Camera: stop()
  Main-->>OS: clean exit
```