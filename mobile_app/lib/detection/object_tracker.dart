import 'dart:math' as math;
import 'dart:ui' show Offset, Rect, Size;

import 'detection_models.dart';

enum TrackingStatus { none, locked, searching }

class TrackingState {
  const TrackingState(this.status, this.box, this.misses);

  static const empty = TrackingState(TrackingStatus.none, null, 0);

  final TrackingStatus status;
  final DetectionBox? box;
  final int misses;

  String get label => switch (status) {
        TrackingStatus.none => 'IDLE',
        TrackingStatus.locked => 'LOCKED',
        TrackingStatus.searching => 'SEARCHING',
      };
}

/// Keeps a lock on one detected object across missed frames without any extra
/// ML model. Association uses predicted position, IoU and a search radius that
/// grows while the target is out of view, so it can re-acquire later.
class ObjectTracker {
  DetectionBox? _box;
  Offset _velocity = Offset.zero;
  int _misses = 0;

  TrackingState get state => TrackingState(
        _box == null
            ? TrackingStatus.none
            : (_misses == 0 ? TrackingStatus.locked : TrackingStatus.searching),
        _box,
        _misses,
      );

  void reset() {
    _box = null;
    _velocity = Offset.zero;
    _misses = 0;
  }

  void lockAt(Offset point, List<DetectionBox> boxes) {
    DetectionBox? best;
    var bestScore = double.negativeInfinity;
    for (final box in boxes) {
      final contains = box.bounds.contains(point) ? 10.0 : 0.0;
      final distance = (box.bounds.center - point).distance;
      final score = contains - distance;
      if (score > bestScore) {
        bestScore = score;
        best = box;
      }
    }
    if (best == null) return;
    _box = best;
    _velocity = Offset.zero;
    _misses = 0;
  }

  TrackingState update(List<DetectionBox> boxes, Size imageSize) {
    final current = _box;
    if (current == null) return TrackingState.empty;

    final diagonal = math.sqrt(
      imageSize.width * imageSize.width + imageSize.height * imageSize.height,
    );
    final predicted = current.bounds.shift(_velocity);
    final threshold = _misses == 0 ? 0.02 : math.min(0.35, 0.05 + 0.03 * _misses);

    DetectionBox? best;
    var bestScore = threshold;
    for (final box in boxes) {
      final score = _matchScore(predicted, box.bounds, diagonal);
      if (score > bestScore) {
        bestScore = score;
        best = box;
      }
    }

    if (best != null) {
      final newCenter = best.bounds.center;
      var velocity = (newCenter - current.bounds.center) * 0.5;
      final speed = velocity.distance;
      final maxSpeed = diagonal * 0.08;
      if (speed > maxSpeed) velocity = velocity / speed * maxSpeed;
      _velocity = velocity;
      _box = _smoothed(current, best);
      _misses = 0;
      return state;
    }

    _misses++;
    var drift = _velocity * 0.5;
    var next = current.bounds.shift(drift);
    next = Rect.fromLTRB(
      next.left.clamp(0.0, imageSize.width),
      next.top.clamp(0.0, imageSize.height),
      next.right.clamp(0.0, imageSize.width),
      next.bottom.clamp(0.0, imageSize.height),
    );
    _velocity = _velocity * 0.8;
    _box = DetectionBox(
      bounds: next,
      label: current.label,
      confidence: current.confidence,
    );
    return state;
  }

  double _matchScore(Rect predicted, Rect candidate, double diagonal) {
    final intersection = (predicted.left < candidate.right &&
            predicted.right > candidate.left &&
            predicted.top < candidate.bottom &&
            predicted.bottom > candidate.top)
        ? (math.min(predicted.right, candidate.right) -
                math.max(predicted.left, candidate.left)) *
            (math.min(predicted.bottom, candidate.bottom) -
                math.max(predicted.top, candidate.top))
        : 0.0;
    final areaA = predicted.width * predicted.height;
    final areaB = candidate.width * candidate.height;
    final union = areaA + areaB - intersection;
    final iou = union <= 0 ? 0.0 : intersection / union;
    final distance =
        (predicted.center - candidate.center).distance / diagonal;
    return iou - distance;
  }

  DetectionBox _smoothed(DetectionBox old, DetectionBox next) {
    const t = 0.6;
    Rect lerp(Rect a, Rect b) => Rect.fromLTRB(
          a.left + (b.left - a.left) * t,
          a.top + (b.top - a.top) * t,
          a.right + (b.right - a.right) * t,
          a.bottom + (b.bottom - a.bottom) * t,
        );
    return DetectionBox(
      bounds: lerp(old.bounds, next.bounds),
      label: next.label,
      confidence: next.confidence,
    );
  }
}
