Implementation plan

[x] Add stream profile model

Define low, medium, and high profiles.
Keep stream settings independent from camera settings.
Do not restart or reconfigure the camera when network quality changes.
[x] Replace synchronous consumer callbacks

Camera acquisition should only publish frames and return quickly.
Move plugin processing to a worker.
Move recording to a worker.
Keep web streaming on its own worker.
[x] Use bounded frame handoff for the web stream

Web UI: one latest-frame mailbox per client.
AI processing: latest-frame mailbox or bounded drop-oldest queue.
Recording: bounded queue if every frame must be preserved.
Never use unbounded queues.
[x] Improve MJPEG streaming

Track frame sequence numbers.
Never encode the same frame twice for one client.
Replace the fixed 50 ms sleep with condition-variable notification and FPS pacing.
Drop stale frames when the client is slow.
[x] Preserve aspect ratio

Resize only the web-stream copy.
Calculate dimensions using the source aspect ratio.
Never independently force width and height.
[x] Add adaptive control

Measure JPEG size, encode duration, send duration, and delivered FPS.
Reduce quality first.
Then reduce resolution.
Reduce FPS last.
Use hysteresis: downgrade quickly, upgrade only after sustained recovery.
[x] Avoid duplicate encoding across clients

Cache encoded JPEG data by source sequence and stream profile:

Clients with the same profile can reuse the same JPEG bytes.
[ ] Add metrics and tests

Test frame dropping.
Test aspect-ratio preservation.
Test quality/profile transitions.
Test slow-client disconnect behavior.
Test that camera acquisition is not blocked by web, recording, or plugin work.
[ ] Evaluate codec later

Adaptive MJPEG is the correct first implementation for the existing <img> UI.
H.264 requires a new browser transport such as fragmented MP4/MSE or HLS.
WebRTC is the best long-term option for true congestion control, but is a major subsystem addition.