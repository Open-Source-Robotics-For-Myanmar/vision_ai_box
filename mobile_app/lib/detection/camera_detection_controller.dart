import 'dart:async';
import 'dart:ui' show Offset, Size;

import 'package:camera/camera.dart';
import 'package:flutter/foundation.dart';
import 'package:permission_handler/permission_handler.dart';

import 'detection_models.dart';
import 'input_image_converter.dart';
import 'object_tracker.dart';
import 'vision_service.dart';

/// What the UI needs from a detection source. Kept as an interface so the
/// screen can be widget-tested without any camera or ML Kit plugins.
abstract class DetectionState extends ChangeNotifier {
  CameraController? get cameraController;
  DetectorKind get kind;
  DetectionFrame? get frame;
  bool get isInitializing;
  bool get isStreaming;
  int get fps;
  String? get error;
  TrackingState get tracking;

  Future<void> initialize();
  Future<void> setKind(DetectorKind kind);
  Future<void> toggleCameraDirection();
  Future<void> pause();
  Future<void> resume();
  Future<void> shutdown();
  void lockTracking(Offset point, Size displaySize);
  void clearTracking();
}

class CameraDetectionController extends DetectionState {
  CameraDetectionController({required this._service});

  final VisionService _service;
  final CameraFrameConverter _converter = CameraFrameConverter();
  final ObjectTracker _tracker = ObjectTracker();

  List<CameraDescription> _cameras = const [];
  int _cameraIndex = 0;
  int _generation = 0;
  int _frameCount = 0;
  int _fpsFrames = 0;
  DateTime _fpsWindow = DateTime.now();
  bool _busy = false;
  bool _initializing = false;
  bool _disposed = false;

  @override
  CameraController? cameraController;

  @override
  DetectorKind kind = DetectorKind.face;

  @override
  DetectionFrame? frame;

  @override
  bool isStreaming = false;

  @override
  int fps = 0;

  @override
  String? error;

  @override
  TrackingState get tracking => _tracker.state;

  @override
  bool get isInitializing => _initializing;

  void _emit() {
    if (!_disposed) notifyListeners();
  }

  @override
  Future<void> initialize() async {
    if (cameraController != null || _initializing) return;
    _initializing = true;
    error = null;
    _emit();

    try {
      final status = await Permission.camera.request();
      if (_disposed) return;
      if (!status.isGranted) {
        error = status.isPermanentlyDenied
            ? 'Camera permission is permanently denied. Enable it in Android settings.'
            : 'Camera permission is required for live detection.';
        return;
      }

      _cameras = await availableCameras();
      if (_disposed) return;
      if (_cameras.isEmpty) {
        error = 'No camera was found on this device.';
        return;
      }
      _cameraIndex = 0;
      await _attach(_cameras.first);
    } on CameraException catch (exception) {
      error = 'Camera error: ${exception.description ?? exception.code}';
    } catch (exception) {
      error = 'Unable to start the camera: $exception';
    } finally {
      _initializing = false;
      _emit();
    }
  }

  Future<void> _attach(CameraDescription description) async {
    final controller = CameraController(
      description,
      ResolutionPreset.veryHigh,
      enableAudio: false,
      imageFormatGroup: ImageFormatGroup.yuv420,
    );
    await controller.initialize();

    ++_generation;
    if (_disposed) {
      await controller.dispose();
      return;
    }

    await controller.startImageStream(_onFrame);
    if (_disposed) {
      await controller.dispose();
      return;
    }

    cameraController = controller;
    frame = null;
    isStreaming = true;
    error = null;
    _emit();
  }

  void _onFrame(CameraImage image) {
    if (_busy || !isStreaming || cameraController == null) return;
    _busy = true;
    _processFrame(image, _generation).whenComplete(() => _busy = false);
  }

  Future<void> _processFrame(CameraImage image, int generation) async {
    final controller = cameraController;
    if (controller == null) return;

    try {
      final converted = _converter.convert(
        image,
        controller.description,
        controller,
        maxDetectionEdge: kind == DetectorKind.face ? 640 : 960,
      );
      if (converted == null) return;

      final started = DateTime.now();
      final boxes = await _service.detect(converted.image, kind);
      if (_disposed || generation != _generation) return;
      _tracker.update(boxes, converted.size);

      frame = DetectionFrame(
        imageSize: converted.size,
        boxes: boxes,
        inferenceTime: DateTime.now().difference(started),
        frameNumber: ++_frameCount,
        mirrored: controller.description.lensDirection == CameraLensDirection.front,
      );
      _recordInference();
      _emit();
    } catch (exception) {
      error = 'Detection failed: $exception';
      _emit();
    }
  }

  void _recordInference() {
    _fpsFrames++;
    final now = DateTime.now();
    final elapsed = now.difference(_fpsWindow).inMilliseconds;
    if (elapsed >= 1000) {
      fps = (_fpsFrames * 1000 ~/ elapsed);
      _fpsFrames = 0;
      _fpsWindow = now;
    }
  }

  @override
  Future<void> setKind(DetectorKind value) async {
    if (kind == value) return;
    kind = value;
    frame = null;
    _emit();
  }

  @override
  Future<void> toggleCameraDirection() async {
    if (_cameras.length < 2) return;
    _generation++;
    final previous = cameraController;
    cameraController = null;
    isStreaming = false;
    frame = null;
    _emit();

    if (previous != null) {
      try {
        await previous.stopImageStream();
      } catch (_) {}
      await previous.dispose();
    }
    if (_disposed) return;

    _cameraIndex = (_cameraIndex + 1) % _cameras.length;
    try {
      await _attach(_cameras[_cameraIndex]);
    } on CameraException catch (exception) {
      error = 'Camera error: ${exception.description ?? exception.code}';
      _emit();
    }
  }

  @override
  Future<void> pause() async {
    final controller = cameraController;
    if (!isStreaming || controller == null) return;
    isStreaming = false;
    _emit();
    try {
      await controller.stopImageStream();
    } catch (_) {}
  }

  @override
  Future<void> resume() async {
    final controller = cameraController;
    if (isStreaming || controller == null || _disposed) return;
    try {
      await controller.startImageStream(_onFrame);
      isStreaming = true;
      _emit();
    } on CameraException catch (exception) {
      error = 'Camera error: ${exception.description ?? exception.code}';
      _emit();
    }
  }

  @override
  void lockTracking(Offset point, Size displaySize) {
    final current = frame;
    if (current == null) return;
    final imagePoint = imagePointFromDisplay(
      point,
      current.imageSize,
      displaySize,
      current.mirrored,
    );
    if (imagePoint == null) return;
    _tracker.lockAt(imagePoint, current.boxes);
    _emit();
  }

  @override
  void clearTracking() {
    _tracker.reset();
    _emit();
  }

  @override
  Future<void> shutdown() async {
    if (_disposed) return;
    _disposed = true;
    _generation++;
    final controller = cameraController;
    cameraController = null;
    isStreaming = false;
    if (controller != null) {
      try {
        await controller.stopImageStream();
      } catch (_) {}
      await controller.dispose();
    }
    await _service.dispose();
    super.dispose();
  }
}
