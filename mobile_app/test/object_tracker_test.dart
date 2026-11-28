import 'dart:ui';

import 'package:flutter_test/flutter_test.dart';
import 'package:vision_ai_box/detection/detection_models.dart';
import 'package:vision_ai_box/detection/object_tracker.dart';

DetectionBox box(double x, double y, [double size = 100]) => DetectionBox(
      bounds: Rect.fromLTWH(x, y, size, size),
      label: 'Object',
      confidence: 0.9,
    );

void main() {
  test('tap locks the box under the finger', () {
    final tracker = ObjectTracker();
    tracker.lockAt(const Offset(150, 150), [box(100, 100), box(500, 500)]);
    expect(tracker.state.status, TrackingStatus.locked);
    expect(tracker.state.box!.bounds.left, 100);
  });

  test('tracks motion, survives a gap and re-acquires the target', () {
    final tracker = ObjectTracker();
    tracker.lockAt(const Offset(150, 150), [box(100, 100)]);
    final size = const Size(1000, 1000);

    final moved = tracker.update([box(140, 100)], size);
    expect(moved.status, TrackingStatus.locked);
    expect(moved.box!.bounds.left, greaterThan(100));

    final lost = tracker.update([], size);
    expect(lost.status, TrackingStatus.searching);
    expect(lost.box, isNotNull);

    // Reappears close to the predicted position.
    final found = tracker.update([box(170, 100)], size);
    expect(found.status, TrackingStatus.locked);
    expect(found.box!.bounds.left, greaterThan(150));
  });

  test('reset clears the lock', () {
    final tracker = ObjectTracker();
    tracker.lockAt(const Offset(150, 150), [box(100, 100)]);
    tracker.reset();
    expect(tracker.state.status, TrackingStatus.none);
    expect(tracker.state.box, isNull);
  });

  test('display tap maps into source pixels with cover crop and mirror', () {
    const source = Size(720, 1280);
    const display = Size(1080, 2200);
    final fitted = fitCoverRect(source, display);
    final center = fitted.center;
    final point = imagePointFromDisplay(center, source, display, false)!;
    expect(point.dx, closeTo(360, 0.001));
    final mirrored = imagePointFromDisplay(center, source, display, true)!;
    expect(mirrored.dx, closeTo(360, 0.001));

    final left = Offset(fitted.left + fitted.width * 0.25, center.dy);
    final direct = imagePointFromDisplay(left, source, display, false)!;
    final flip = imagePointFromDisplay(left, source, display, true)!;
    expect(direct.dx, closeTo(180, 0.001));
    expect(flip.dx, closeTo(540, 0.001));
  });
}
