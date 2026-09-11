# Vision AI Box - Deep Blue AI Lab

docker run --rm -it \
  --privileged \
  --network host \
  -v /home/mr_robot/Desktop/core3:/home/ghost/core3 \
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

### State Workflow

```mermaid
stateDiagram-v2
    [*] --> Start

    state "main()" as Main {
        [*] --> InitToggles
        InitToggles --> SetCamera
        SetCamera --> USB: CAMERA_USB
        SetCamera --> RealSense: CAMERA_REALSENSE
        USB --> ServerStart
        RealSense --> ServerStart
        ServerStart --> WebReady: web_server.start(port)
        WebReady --> InitialCameraCheck: if camera.check_device_state()
        InitialCameraCheck --> CameraActive: if camera.start()
        InitialCameraCheck --> CameraOffline: else
        CameraActive --> MonitorThread: std::thread(camera_reconnect_loop)
        CameraOffline --> MonitorThread
        MonitorThread --> ServiceLoop
    }

    ServiceLoop --> Sleep250: while toggles.running
    Sleep250 --> ServiceLoop

    ServiceLoop --> SignalStop: SIGINT / SIGTERM -> handle_signal()
    SignalStop --> DisableFlags: toggles.running=false; camera_enabled=false; processing_enabled=false; camera_error=false
    DisableFlags --> StopWeb: web_server.stop()
    StopWeb --> StopCamera: camera.set_processing_enabled(false); camera.stop()
    StopCamera --> JoinThread: join camera_monitor_thread
    JoinThread --> [*]

    state "camera_reconnect_loop()" as Reconnect {
        [*] --> Loop
        Loop --> CheckEnabled: while toggles.running
        CheckEnabled --> Disabled: if !toggles.camera_enabled
        Disabled --> StopIfRunning: if camera.is_running()
        StopIfRunning --> ResetDisabled: camera.stop(); toggles.camera_error=false; toggles.processing_enabled=false; camera.set_processing_enabled(false)
        ResetDisabled --> LoopWait

        CheckEnabled --> LostWhileRunning: else if camera.is_running() && !camera.check_device_state()
        LostWhileRunning --> StopCameraAndFlag: camera.stop(); toggles.camera_error=true; toggles.processing_enabled=false; camera.set_processing_enabled(false)
        StopCameraAndFlag --> LoopWait

        CheckEnabled --> StartWhenReady: else if !camera.is_running() && camera.check_device_state()
        StartWhenReady --> TryStart: if camera.start()
        TryStart --> EnableCamera: toggles.camera_error=false; toggles.processing_enabled=true; camera.set_processing_enabled(true)
        EnableCamera --> LoopWait
        TryStart --> StartFailed: else
        StartFailed --> FlagError: toggles.camera_error=true; toggles.processing_enabled=false; camera.set_processing_enabled(false)
        FlagError --> LoopWait

        CheckEnabled --> NoDevice: else if !camera.check_device_state()
        NoDevice --> FlagError2: toggles.camera_error=true; toggles.processing_enabled=false; camera.set_processing_enabled(false)
        FlagError2 --> LoopWait

        LoopWait --> Loop: sleep_for(250ms / 2s)
    }

    state "WebServer::accept_loop()" as Accept {
        [*] --> WaitForClient
        WaitForClient --> AcceptClient: accept()
        AcceptClient --> SpawnHandler: std::thread(handle_client)
        SpawnHandler --> WaitForClient
    }

    state "WebServer::handle_client()" as Client {
        [*] --> ReadHeaders
        ReadHeaders --> ParseRequest: request header complete
        ParseRequest --> LoginRoute: if path == "/api/login"
        LoginRoute --> ValidateCreds: credentials valid?
        ValidateCreds --> SetSession: session created; Set-Cookie
        ValidateCreds --> LoginError: else
        LoginError --> CloseClient

        ParseRequest --> CheckAuth: else if !is_authenticated(request)
        CheckAuth --> AuthError: HTTP 401
        CheckAuth --> Dispatch: else

        Dispatch --> Dashboard: GET /dashboard
        Dispatch --> Logs: GET /api/logs
        Dispatch --> RecordStatus: GET /api/record/status
        Dispatch --> RecordStart: POST /api/record/start
        Dispatch --> RecordStop: POST /api/record/stop
        Dispatch --> RecordRestart: POST /api/record/restart
        Dispatch --> Capture: POST /api/record/capture
        Dispatch --> SettingsGET: GET /api/camera/settings
        Dispatch --> SettingsPOST: POST /api/camera/settings
        Dispatch --> CameraStatus: GET /api/camera/status
        Dispatch --> CameraStart: POST /api/camera/start
        Dispatch --> CameraStop: POST /api/camera/stop
        Dispatch --> Stream: GET /api/camera/stream
        Dispatch --> NotFound: default

        Dashboard --> CloseClient
        Logs --> CloseClient
        RecordStatus --> CloseClient
        RecordStart --> CloseClient
        RecordStop --> CloseClient
        RecordRestart --> CloseClient
        Capture --> CloseClient
        SettingsGET --> CloseClient
        SettingsPOST --> CloseClient
        CameraStatus --> CloseClient
        CameraStart --> CloseClient
        CameraStop --> CloseClient
        Stream --> CloseClient
        AuthError --> CloseClient
        NotFound --> CloseClient
    }

    state "WebServer::start_camera()" as StartCamera {
        [*] --> StartRequest
        StartRequest --> SetEnabled: toggles.camera_enabled = true
        SetEnabled --> AlreadyRunning: if camera.is_running()
        AlreadyRunning --> EnableProcessing: toggles.processing_enabled = true; camera.set_processing_enabled(true)
        EnableProcessing --> Done

        SetEnabled --> DeviceMissing: else if !camera.check_device_state()
        DeviceMissing --> StartFailed: toggles.camera_error = true; toggles.processing_enabled = false
        StartFailed --> Done

        SetEnabled --> TryStart: else if !camera.start()
        TryStart --> StartFailed2: toggles.camera_error = true; toggles.processing_enabled = false
        StartFailed2 --> Done

        TryStart --> StartSucceeded: else
        StartSucceeded --> EnableProcessing2: toggles.camera_error = false; toggles.processing_enabled = true; camera.set_processing_enabled(true)
        EnableProcessing2 --> Done
    }

    state "WebServer::stop_camera()" as StopCamera2 {
        [*] --> StopRequest
        StopRequest --> ClearFlags: toggles.camera_enabled = false; toggles.processing_enabled = false; toggles.camera_error = false
        ClearFlags --> StopIfRunning: if camera.is_running()
        StopIfRunning --> DisableAndStop: camera.set_processing_enabled(false); camera.stop()
        DisableAndStop --> DoneStop
    }

    state "BaseSystem::start_recording()/stop_recording()/restart_recording()/capture_frame()" as Recording {
        [*] --> Idle
        Idle --> StartReq: POST /api/record/start or start_recording()
        StartReq --> AlreadyRecording: if recording_active_
        AlreadyRecording --> Idle: return true

        StartReq --> PrepareVideoDir: create_directories("media/videos")
        PrepareVideoDir --> SetActive: recording_active_ = true; last_written_sequence_ = 0
        SetActive --> Ready

        Ready --> FrameEvent: on_frame_received(frame)
        FrameEvent --> IgnoreInactive: if !recording_active_ || frame.color empty
        IgnoreInactive --> Ready
        FrameEvent --> IgnoreDuplicate: else if sequence != 0 && sequence <= last_written_sequence_
        IgnoreDuplicate --> Ready
        FrameEvent --> EnsureWriter: else
        EnsureWriter --> WriterOpen: if writer_.isOpened()
        WriterOpen --> WriteFrame: writer_.write(*frame.color)
        WriteFrame --> UpdateSeq: last_written_sequence_ = sequence
        UpdateSeq --> Ready
        EnsureWriter --> NeedOpen: else if !writer_.isOpened()
        NeedOpen --> TryOpen: try_open_writer(...)
        TryOpen --> WriterOK: if success
        WriterOK --> WriteFrame
        TryOpen --> WriterFail: else
        WriterFail --> DisableRecord: recording_active_ = false
        DisableRecord --> Ready

        Ready --> StopReq: POST /api/record/stop or stop_recording()
        StopReq --> CloseWriter: close_writer(); recording_active_ = false
        CloseWriter --> Idle

        Idle --> RestartReq: POST /api/record/restart
        RestartReq --> StopReq

        Idle --> CaptureReq: POST /api/record/capture
        CaptureReq --> MakeDir: create_directories("media/image")
        MakeDir --> FetchFrame: if !camera.latest_frame(frame) || frame.color empty
        FetchFrame --> CaptureFail: return false
        FetchFrame --> SpawnSaveThread: std::thread(imwrite)
        SpawnSaveThread --> Idle
    }

    state "UsbCamera::acquisition_loop()" as USBLoop {
        [*] --> Wait
        Wait --> CheckProcessing: while running_
        CheckProcessing --> Sleep: if !processing_enabled_
        Sleep --> Wait
        CheckProcessing --> ReadFrame: else
        ReadFrame --> ValidFrame: if capture_.read(frame) && !frame.empty()
        ValidFrame --> Publish: next.sequence = ++sequence; latest_frame_ = next.color; notify callbacks
        Publish --> Wait
        ReadFrame --> HotUnplug: else
        HotUnplug --> StopReader: running_=false; capture_.release(); initialized_=false
        StopReader --> Wait
    }

    state "RealSenseCamera::acquisition_loop()" as RSLoop {
        [*] --> WaitRS
        WaitRS --> CheckEnabledRS: while running_
        CheckEnabledRS --> SleepRS: if !processing_enabled_
        SleepRS --> WaitRS
        CheckEnabledRS --> PollFrames: else
        PollFrames --> TryFrames: if pipeline_.try_wait_for_frames(...)
        TryFrames --> Align: if depth_enabled_ && align_to_color_
        Align --> ColorRead: raw_color = get_color_frame()
        ColorRead --> ValidRS: if !raw_color.empty()
        ValidRS --> PublishRS: next.sequence = ++sequence; latest_frame_ = next.color; callbacks(...)
        PublishRS --> WaitRS
        TryFrames --> ContinueRS: else
        ContinueRS --> WaitRS
        PollFrames --> ExceptionRS: catch rs2::error / std::exception
        ExceptionRS --> HotUnplugRS: log "Hot unplug detected"
        HotUnplugRS --> WaitRS
    }
```

## Implementation Diagrams

### Worker Threads