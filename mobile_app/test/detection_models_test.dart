import 'dart:ui';

import 'package:camera/camera.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:flutter/services.dart' show DeviceOrientation;
import 'package:vision_ai_box/detection/detection_models.dart';
import 'package:vision_ai_box/detection/input_image_converter.dart';

void main() {
  test('oriented size swaps width and height for quarter turns', () {
    expect(orientedSize(640, 480, swapped: true), const Size(480, 640));
    expect(orientedSize(640, 480, swapped: false), const Size(640, 480));
  });

  test('detection boxes map proportionally into the preview', () {
    final box = DetectionBox(
      bounds: const Rect.fromLTWH(160, 120, 160, 120),
      label: 'Face',
      confidence: 1,
    );
    expect(
      mapDetectionRect(box.bounds, const Size(640, 480), const Size(320, 240)),
      const Rect.fromLTWH(80, 60, 80, 60),
    );
  });

  test('detection input is downscaled to an even-sized long edge', () {
    expect(scaledForDetection(1920, 1080, 1280), const Size(1280, 720));
    expect(scaledForDetection(1280, 720, 1280), const Size(1280, 720));
    expect(scaledForDetection(1000, 501, 1280), const Size(1000, 500));
  });

  test('cover fit centers and crops the source without distortion', () {
    final rect = fitCoverRect(const Size(720, 1280), const Size(1080, 2200));
    expect(rect.height, closeTo(2200, 0.001));
    expect(rect.width, closeTo(1237.5, 0.001));
    expect(rect.center.dx, closeTo(540, 0.001));
  });

  test('sensor and device rotation are combined per lens direction', () {
    expect(
      rawRotationFor(
        sensorOrientation: 90,
        lensDirection: CameraLensDirection.front,
        deviceOrientation: DeviceOrientation.portraitUp,
      ),
      90,
    );
    expect(
      rawRotationFor(
        sensorOrientation: 90,
        lensDirection: CameraLensDirection.back,
        deviceOrientation: DeviceOrientation.landscapeLeft,
      ),
      0,
    );
  });

  test('front-camera boxes mirror around the image center', () {
    final box = Rect.fromLTWH(10, 20, 30, 40);
    expect(
      mirrorRectHorizontally(box, 100),
      const Rect.fromLTWH(160, 20, 30, 40),
    );
  });

  test('detector labels describe the two supported models', () {
    expect(DetectorKind.face.label, 'Face');
    expect(DetectorKind.object.label, 'Object');
    expect(DetectorKind.face.modelName, contains('BlazeFace'));
    expect(DetectorKind.object.modelName, contains('MobileNet SSD'));
  });
}
