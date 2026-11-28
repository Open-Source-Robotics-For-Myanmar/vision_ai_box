import 'dart:typed_data';
import 'dart:ui' show Size;

import 'package:camera/camera.dart';
import 'package:flutter/services.dart' show DeviceOrientation;
import 'package:google_mlkit_commons/google_mlkit_commons.dart';

/// Pure rotation math so it can be unit-tested without a camera.
/// Returns clockwise degrees in the image coordinate space.
int rawRotationFor({
  required int sensorOrientation,
  required CameraLensDirection lensDirection,
  required DeviceOrientation deviceOrientation,
}) {
  final deviceDegrees = switch (deviceOrientation) {
    DeviceOrientation.portraitUp => 0,
    DeviceOrientation.landscapeLeft => 90,
    DeviceOrientation.portraitDown => 180,
    DeviceOrientation.landscapeRight => 270,
  };
  return lensDirection == CameraLensDirection.front
      ? (sensorOrientation + deviceDegrees) % 360
      : (sensorOrientation - deviceDegrees + 360) % 360;
}

bool isQuarterTurn(InputImageRotation rotation) =>
    rotation == InputImageRotation.rotation90deg ||
    rotation == InputImageRotation.rotation270deg;

/// Downscale target with the same aspect ratio and even dimensions (NV21 needs pairs).
Size scaledForDetection(int width, int height, int maxEdge) {
  var w = width;
  var h = height;
  if (maxEdge > 0 && (width > maxEdge || height > maxEdge)) {
    final scale = maxEdge / (width > height ? width : height);
    w = (width * scale).toInt();
    h = (height * scale).toInt();
  }
  if (w.isOdd) w--;
  if (h.isOdd) h--;
  return Size(w < 2 ? 2 : w.toDouble(), h < 2 ? 2 : h.toDouble());
}

class ConvertedCameraFrame {
  const ConvertedCameraFrame({
    required this.image,
    required this.size,
    required this.rotation,
  });

  final InputImage image;

  /// Size in the rotated coordinate space ML Kit uses for bounding boxes.
  final Size size;
  final InputImageRotation rotation;
}

/// Packs camera frames into NV21, optionally downscaling for detection while
/// leaving the higher-resolution camera preview untouched. Buffers are reused.
class CameraFrameConverter {
  Uint8List? _nv21;
  Uint8List? _scaled;
  Int32List? _yMapX;
  Int32List? _yMapY;
  Int32List? _uvMapX;
  Int32List? _uvMapY;
  int _mapWidth = 0;
  int _mapHeight = 0;

  InputImageRotation? rotationFor(CameraDescription camera, CameraController controller) {
    final orientation = controller.value.deviceOrientation;
    return InputImageRotationValue.fromRawValue(rawRotationFor(
      sensorOrientation: camera.sensorOrientation,
      lensDirection: camera.lensDirection,
      deviceOrientation: orientation,
    ));
  }

  ConvertedCameraFrame? convert(
    CameraImage image,
    CameraDescription camera,
    CameraController controller, {
    int maxDetectionEdge = 1280,
  }) {
    final rotation = rotationFor(camera, controller);
    final formatValue = InputImageFormatValue.fromRawValue(image.format.raw);
    if (rotation == null || formatValue == null) return null;

    final width = image.width;
    final height = image.height;
    final required = width * height * 3 ~/ 2;
    if (_nv21 == null || _nv21!.length != required) {
      _nv21 = Uint8List(required);
    }
    final full = _nv21!;

    if (image.planes.length == 1) {
      full.setAll(0, image.planes.first.bytes);
    } else if (image.planes.length == 3 &&
        (formatValue == InputImageFormat.yuv_420_888 ||
            formatValue == InputImageFormat.yv12)) {
      _packNv21(image, full);
    } else {
      return null;
    }

    var bytes = full;
    var target = scaledForDetection(width, height, maxDetectionEdge);
    final tw = target.width.toInt();
    final th = target.height.toInt();
    if (tw != width || th != height) {
      final scaledSize = tw * th * 3 ~/ 2;
      if (_scaled == null || _scaled!.length != scaledSize) {
        _scaled = Uint8List(scaledSize);
      }
      _downscale(full, width, height, _scaled!, tw, th);
      bytes = _scaled!;
    }

    final oriented = isQuarterTurn(rotation) ? Size(th.toDouble(), tw.toDouble()) : target;
    return ConvertedCameraFrame(
      image: InputImage.fromBytes(
        bytes: bytes,
        metadata: InputImageMetadata(
          size: Size(tw.toDouble(), th.toDouble()),
          rotation: rotation,
          // CameraX reports raw=35 for NV21 single-plane frames.
          format: InputImageFormat.nv21,
          bytesPerRow: tw,
        ),
      ),
      size: oriented,
      rotation: rotation,
    );
  }

  void _packNv21(CameraImage image, Uint8List out) {
    final width = image.width;
    final height = image.height;
    final ySize = width * height;
    final y = image.planes[0];
    final rowLength = width < y.bytesPerRow ? width : y.bytesPerRow;
    for (var row = 0; row < height; row++) {
      out.setRange(row * width, row * width + rowLength, y.bytes,
          row * y.bytesPerRow);
    }

    final u = image.planes[1];
    final v = image.planes[2];
    final uStep = u.bytesPerPixel ?? 1;
    final vStep = v.bytesPerPixel ?? 1;
    var index = ySize;
    for (var row = 0; row < height ~/ 2; row++) {
      for (var col = 0; col < width ~/ 2; col++) {
        out[index++] =
            v.bytes[row * v.bytesPerRow + col * vStep]; // V first = NV21
        out[index++] = u.bytes[row * u.bytesPerRow + col * uStep];
      }
    }
  }

  void _downscale(Uint8List src, int sw, int sh, Uint8List dst, int dw, int dh) {
    if (_mapWidth != sw || _mapHeight != sh ||
        _yMapX == null || _yMapX!.length != dw || _uvMapX == null ||
        _uvMapX!.length != dw ~/ 2) {
      _yMapX = Int32List.fromList(List<int>.generate(dw, (x) => (x * sw / dw).toInt().clamp(0, sw - 1)));
      _yMapY = Int32List.fromList(List<int>.generate(dh, (y) => (y * sh / dh).toInt().clamp(0, sh - 1)));
      _uvMapX = Int32List.fromList(List<int>.generate(
          dw ~/ 2, (x) => (x * (sw ~/ 2) / (dw ~/ 2)).toInt().clamp(0, sw ~/ 2 - 1)));
      _uvMapY = Int32List.fromList(List<int>.generate(
          dh ~/ 2, (y) => (y * (sh ~/ 2) / (dh ~/ 2)).toInt().clamp(0, sh ~/ 2 - 1)));
      _mapWidth = sw;
      _mapHeight = sh;
    }
    final yMapX = _yMapX!;
    final yMapY = _yMapY!;
    final uvMapX = _uvMapX!;
    final uvMapY = _uvMapY!;
    final ySize = dw * dh;
    for (var y = 0; y < dh; y++) {
      final rowOffset = yMapY[y] * sw;
      final outOffset = y * dw;
      for (var x = 0; x < dw; x++) {
        dst[outOffset + x] = src[rowOffset + yMapX[x]];
      }
    }
    for (var row = 0; row < dh ~/ 2; row++) {
      final srcRow = sw * sh + uvMapY[row] * sw;
      final outRow = ySize + row * dw;
      for (var col = 0; col < dw ~/ 2; col++) {
        final srcIndex = srcRow + uvMapX[col] * 2;
        final dstIndex = outRow + col * 2;
        dst[dstIndex] = src[srcIndex];
        dst[dstIndex + 1] = src[srcIndex + 1];
      }
    }
  }
}
