# vision_ai_box — deepblueailab (Android)

On-device camera app for Deep Blue AI Lab. It replaces the desktop RetinaFace and
YOLO26 plugins with mobile-friendly bundled ML Kit models:

| Desktop plugin | Android replacement |
| --- | --- |
| `face_detection` (RetinaFace ONNX, 53 MB) | ML Kit Face Detection (BlazeFace-style, bundled) |
| `object_detection` (YOLO26l ONNX) | ML Kit Object Detection (MobileNet-SSD based, bundled, COCO labels) |

Everything runs locally on the phone: no HTTP server, no Firebase, no account,
no network needed after install.

## Features

- Live camera preview (front/back switch), with permission handling.
- Two detection modes: **Face** and **Object**, switchable at runtime.
- Bounding boxes with labels/confidence drawn over the preview.
- Inference FPS and latency indicators; pause/resume detection.
- Dark UI matching the desktop dashboard's DeepBlue theme.

## Build

Flutter 3.50.1+ and Android SDK are required. The app id is
`com.deepblueailab.vision_ai_box`, minimum SDK 24.

```sh
flutter pub get
flutter analyze
flutter test
flutter build apk --debug
```

`android/gradle.properties` sets `android.builder.sdkDownload=false` so Gradle
does not re-run sdkmanager on a machine whose SDK is already installed.

## Notes

- Detector models are bundled with the ML Kit libraries (not downloaded on
  first use), so inference works offline immediately. The trade-off is a larger
  APK.
- Camera frames arrive as YUV/NV21; `lib/detection/input_image_converter.dart`
  packs them into the NV21 byte array `InputImage` requires and applies the
  sensor/device rotation.
- `DetectionState` is an interface so the screen can be widget-tested without
  camera or ML Kit plugins.
