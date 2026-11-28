import 'dart:ui' show Offset, Rect, Size;

/// The two on-device detectors supported by the Android app.
enum DetectorKind { face, object }

extension DetectorKindInfo on DetectorKind {
  String get label => switch (this) {
        DetectorKind.face => 'Face',
        DetectorKind.object => 'Object',
      };

  String get modelName => switch (this) {
        DetectorKind.face => 'ML Kit Face (BlazeFace)',
        DetectorKind.object => 'ML Kit Object (MobileNet SSD)',
      };
}

/// One detected box, already expressed in the oriented camera-frame space.
class DetectionBox {
  const DetectionBox({
    required this.bounds,
    required this.label,
    required this.confidence,
  });

  final Rect bounds;
  final String label;
  final double confidence;

  @override
  bool operator ==(Object other) => other is DetectionBox &&
      other.bounds == bounds &&
      other.label == label &&
      other.confidence == confidence;

  @override
  int get hashCode => Object.hash(bounds, label, confidence);
}

/// A single inference result together with its timing information.
class DetectionFrame {
  const DetectionFrame({
    required this.imageSize,
    required this.boxes,
    required this.inferenceTime,
    required this.frameNumber,
    this.mirrored = false,
  });

  /// Size of the image AFTER the sensor rotation has been applied.
  final Size imageSize;
  final List<DetectionBox> boxes;
  final Duration inferenceTime;
  final int frameNumber;

  /// CameraX mirrors the front-camera preview but not its analysis frames.
  final bool mirrored;

  int get count => boxes.length;
}

/// ML Kit reports boxes in the rotated image coordinate space.
Size orientedSize(int width, int height, {required bool swapped}) =>
    swapped ? Size(height.toDouble(), width.toDouble())
            : Size(width.toDouble(), height.toDouble());

/// Rect a [source] image occupies when it covers [target] (BoxFit.cover).
Rect fitCoverRect(Size source, Size target) {
  if (source.width <= 0 || source.height <= 0) return Rect.zero;
  final scale = target.width / source.width > target.height / source.height
      ? target.width / source.width
      : target.height / source.height;
  final width = source.width * scale;
  final height = source.height * scale;
  return Rect.fromLTWH(
    (target.width - width) / 2,
    (target.height - height) / 2,
    width,
    height,
  );
}

/// Maps a box from the source image space onto the widget that paints it.
/// Converts a tap on the rendered preview back into source-image pixels.
Offset? imagePointFromDisplay(
  Offset point,
  Size source,
  Size display,
  bool mirrored,
) {
  final fitted = fitCoverRect(source, display);
  if (!fitted.contains(point)) return null;
  var x = (point.dx - fitted.left) / fitted.width * source.width;
  final y = (point.dy - fitted.top) / fitted.height * source.height;
  if (mirrored) x = source.width - x;
  return Offset(
    x.clamp(0, source.width),
    y.clamp(0, source.height),
  );
}

/// Reflects a rect around a vertical axis (used to match the mirrored preview).
Rect mirrorRectHorizontally(Rect rect, double axisX) => Rect.fromLTRB(
      2 * axisX - rect.right,
      rect.top,
      2 * axisX - rect.left,
      rect.bottom,
    );

Rect mapDetectionRect(Rect box, Size source, Size target) {
  if (source.width <= 0 || source.height <= 0) return Rect.zero;
  return Rect.fromLTRB(
    box.left / source.width * target.width,
    box.top / source.height * target.height,
    box.right / source.width * target.width,
    box.bottom / source.height * target.height,
  );
}
