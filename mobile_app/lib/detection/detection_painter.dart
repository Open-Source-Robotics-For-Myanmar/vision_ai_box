import 'package:material_ui/material_ui.dart';

import 'detection_models.dart';
import 'object_tracker.dart';

/// Draws ML Kit boxes on top of the camera preview.
class DetectionPainter extends CustomPainter {
  const DetectionPainter({
    this.frame,
    this.tracking,
    required this.accent,
  });

  final DetectionFrame? frame;
  final TrackingState? tracking;
  final Color accent;

  @override
  void paint(Canvas canvas, Size size) {
    final current = frame;
    if (current == null) return;

    final fitted = fitCoverRect(current.imageSize, size);
    final outline = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = 2
      ..color = accent;
    final chipPaint = Paint()..color = accent.withValues(alpha: 0.78);

    for (final box in current.boxes) {
      var rect = mapDetectionRect(box.bounds, current.imageSize, fitted.size)
          .shift(fitted.topLeft);
      if (current.mirrored) {
        rect = mirrorRectHorizontally(rect, fitted.center.dx);
      }
      canvas.drawRect(rect, outline);

      final label = box.confidence >= 1
          ? box.label
          : '${box.label} ${(box.confidence * 100).round()}%';
      final text = TextPainter(
        text: TextSpan(
          text: label,
          style: const TextStyle(color: Color(0xFFFFFFFF), fontSize: 11),
        ),
        textDirection: TextDirection.ltr,
      )..layout();

      final chipTop = rect.top >= 20 ? rect.top - 18 : rect.top;
      final chip = Rect.fromLTWH(rect.left, chipTop, text.width + 10, 18);
      canvas.drawRect(chip, chipPaint);
      text.paint(canvas, Offset(chip.left + 5, chip.top + 3));
    }

    final locked = tracking?.box;
    if (locked != null) {
      var rect = mapDetectionRect(locked.bounds, current.imageSize, fitted.size)
          .shift(fitted.topLeft);
      if (current.mirrored) {
        rect = mirrorRectHorizontally(rect, fitted.center.dx);
      }
      final trackingPaint = Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = 4
        ..color = const Color(0xFFFFB300);
      canvas.drawRect(rect, trackingPaint);
      final label = TextPainter(
        text: TextSpan(
          text: 'TRACK ${tracking!.label}',
          style: const TextStyle(
            color: Color(0xFFFFB300),
            fontSize: 12,
            fontWeight: FontWeight.bold,
          ),
        ),
        textDirection: TextDirection.ltr,
      )..layout();
      label.paint(canvas, Offset(rect.left, rect.top > 18 ? rect.top - 18 : rect.top));
    }
  }

  @override
  bool shouldRepaint(covariant DetectionPainter old) =>
      old.frame != frame || old.tracking != tracking || old.accent != accent;
}
