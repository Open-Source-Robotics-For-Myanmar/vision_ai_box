import 'package:flutter_test/flutter_test.dart';
import 'package:material_ui/material_ui.dart';
import 'dart:ui' show Offset, Size;

import 'package:camera/camera.dart';
import 'package:vision_ai_box/detection/camera_detection_controller.dart';
import 'package:vision_ai_box/detection/detection_models.dart';
import 'package:vision_ai_box/detection/object_tracker.dart';
import 'package:vision_ai_box/screens/detection_screen.dart';

class FakeDetectionState extends DetectionState {
  @override
  CameraController? get cameraController => null;

  @override
  DetectorKind kind = DetectorKind.face;

  @override
  DetectionFrame? frame;

  @override
  bool get isInitializing => false;

  @override
  bool get isStreaming => false;

  @override
  int get fps => 0;

  @override
  String? get error => null;

  @override
  TrackingState get tracking => TrackingState.empty;

  @override
  Future<void> initialize() async {}

  @override
  Future<void> setKind(DetectorKind value) async {
    kind = value;
    notifyListeners();
  }

  @override
  Future<void> toggleCameraDirection() async {}

  @override
  Future<void> pause() async {}

  @override
  Future<void> resume() async {}

  @override
  Future<void> shutdown() async {
    dispose();
  }

  @override
  void lockTracking(Offset point, Size displaySize) {}

  @override
  void clearTracking() {}
}

void main() {
  testWidgets('detection screen shows both modes and switches them',
      (tester) async {
    final state = FakeDetectionState();
    await tester.pumpWidget(MaterialApp(home: DetectionScreen(controller: state)));
    await tester.pump();

    expect(find.text('DeepBlue Vision'), findsOneWidget);
    expect(find.text('Starting camera…'), findsOneWidget);
    expect(find.text('Face'), findsOneWidget);
    expect(find.text('Object'), findsOneWidget);

    await tester.tap(find.text('Object'));
    await tester.pump();
    expect(state.kind, DetectorKind.object);
  });
}
