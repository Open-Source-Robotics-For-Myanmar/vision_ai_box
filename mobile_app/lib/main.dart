import 'package:material_ui/material_ui.dart';

import 'detection/camera_detection_controller.dart';
import 'detection/vision_service.dart';
import 'screens/detection_screen.dart';

void main() => runApp(const VisionApp());

class VisionApp extends StatefulWidget {
  const VisionApp({super.key});

  @override
  State<VisionApp> createState() => _VisionAppState();
}

class _VisionAppState extends State<VisionApp> {
  final CameraDetectionController controller = CameraDetectionController(
    service: VisionService(),
  );

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'VAB',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: const Color(0xFF1E88E5),
          brightness: Brightness.dark,
        ),
        useMaterial3: true,
      ),
      home: DetectionScreen(controller: controller),
    );
  }
}
