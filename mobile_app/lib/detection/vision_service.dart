import 'package:google_mlkit_face_detection/google_mlkit_face_detection.dart';
import 'package:google_mlkit_object_detection/google_mlkit_object_detection.dart';

import 'detection_models.dart';

/// Owns the bundled (offline) ML Kit detectors.
class VisionService {
  FaceDetector? _faceDetector;
  ObjectDetector? _objectDetector;

  Future<List<DetectionBox>> detect(InputImage image, DetectorKind kind) async {
    switch (kind) {
      case DetectorKind.face:
        final detector = _faceDetector ??= FaceDetector(
          options: FaceDetectorOptions(
            performanceMode: FaceDetectorMode.fast,
            minFaceSize: 0.1,
          ),
        );
        final faces = await detector.processImage(image);
        return faces
            .map((face) => DetectionBox(
                  bounds: face.boundingBox,
                  label: 'Face',
                  confidence: 1,
                ))
            .toList(growable: false);
      case DetectorKind.object:
        final detector = _objectDetector ??= ObjectDetector(
          options: ObjectDetectorOptions(
            mode: DetectionMode.stream,
            classifyObjects: true,
            multipleObjects: true,
          ),
        );
        final objects = await detector.processImage(image);
        return objects
            .where((object) => object.labels.isNotEmpty)
            .map((object) {
              final label = object.labels.first;
              return DetectionBox(
                bounds: object.boundingBox,
                label: label.text,
                confidence: label.confidence,
              );
            })
            .toList(growable: false);
    }
  }

  Future<void> dispose() async {
    await _faceDetector?.close();
    await _objectDetector?.close();
    _faceDetector = null;
    _objectDetector = null;
  }
}
