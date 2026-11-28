import 'dart:async';

import 'package:camera/camera.dart';
import 'package:flutter/services.dart' show DeviceOrientation;
import 'package:material_ui/material_ui.dart';

import '../detection/camera_detection_controller.dart';
import '../detection/detection_models.dart';
import '../detection/detection_painter.dart';
import '../detection/object_tracker.dart';

class DetectionScreen extends StatefulWidget {
  const DetectionScreen({super.key, required this.controller});

  final DetectionState controller;

  @override
  State<DetectionScreen> createState() => _DetectionScreenState();
}

class _DetectionScreenState extends State<DetectionScreen>
    with WidgetsBindingObserver {
  late final DetectionState _state = widget.controller;
  static const Color _accent = Color(0xFF4FC3F7);

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    _state.addListener(_onStateChanged);
    unawaited(_state.initialize());
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    _state.removeListener(_onStateChanged);
    super.dispose();
    unawaited(_state.shutdown());
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState lifecycle) {
    if (lifecycle == AppLifecycleState.paused ||
        lifecycle == AppLifecycleState.inactive ||
        lifecycle == AppLifecycleState.detached) {
      unawaited(_state.pause());
    } else if (lifecycle == AppLifecycleState.resumed) {
      unawaited(_state.resume());
    }
  }

  void _onStateChanged() {
    if (mounted) setState(() {});
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      backgroundColor: const Color(0xFF121212),
      appBar: AppBar(
        backgroundColor: const Color(0xFF000000),
        foregroundColor: Colors.white,
        bottom: PreferredSize(
          preferredSize: const Size(0, 2),
          child: ColoredBox(color: Color(0xFF1E88E5), child: SizedBox(height: 2)),
        ),
        title: const Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Text(
              'VAB',
              style: TextStyle(fontSize: 18, fontWeight: FontWeight.w700),
            ),
            Text(
              'DeepBlue Vision · deepblueailab',
              style: TextStyle(fontSize: 10, color: Color(0xFFA0A0A0)),
            ),
          ],
        ),
        actions: [
          IconButton(
            tooltip: 'Switch camera',
            icon: const Icon(Icons.cameraswitch_outlined),
            onPressed: _state.cameraController == null
                ? null
                : () => unawaited(_state.toggleCameraDirection()),
          ),
          IconButton(
            tooltip: _state.isStreaming ? 'Pause detection' : 'Resume detection',
            icon: Icon(_state.isStreaming ? Icons.pause_circle_outline : Icons.play_circle_outline),
            onPressed: _state.cameraController == null
                ? null
                : () => unawaited(
                      _state.isStreaming ? _state.pause() : _state.resume(),
                    ),
          ),
        ],
      ),
      body: SafeArea(child: _buildBody()),
      bottomNavigationBar: _buildControls(),
    );
  }

  Widget _buildBody() {
    final error = _state.error;
    if (error != null) {
      return Center(
        child: Padding(
          padding: const EdgeInsets.all(24),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              const Icon(Icons.videocam_off_outlined,
                  size: 48, color: Color(0xFFFFB300)),
              const SizedBox(height: 12),
              Text(error, textAlign: TextAlign.center),
              const SizedBox(height: 16),
              FilledButton(
                onPressed: () => unawaited(_state.initialize()),
                child: const Text('Retry'),
              ),
            ],
          ),
        ),
      );
    }

    final controller = _state.cameraController;
    if (controller == null || !controller.value.isInitialized) {
      return const Center(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            CircularProgressIndicator(color: _accent),
            SizedBox(height: 14),
            Text('Starting camera…'),
          ],
        ),
      );
    }

    return LayoutBuilder(
      builder: (context, constraints) {
        final displaySize = Size(constraints.maxWidth, constraints.maxHeight);
        return GestureDetector(
          onTapDown: (details) =>
              _state.lockTracking(details.localPosition, displaySize),
          child: _previewStack(controller, displaySize),
        );
      },
    );
  }

  Widget _previewStack(CameraController controller, Size displaySize) {
    return Stack(
      fit: StackFit.expand,
      children: [
        SizedBox.expand(
          child: FittedBox(
            fit: BoxFit.cover,
            child: SizedBox(
              width: _previewDisplaySize(controller).width,
              height: _previewDisplaySize(controller).height,
              child: CameraPreview(controller),
            ),
          ),
        ),
        CustomPaint(
          painter: DetectionPainter(
            frame: _state.frame,
            tracking: _state.tracking,
            accent: _accent,
          ),
        ),
        const Positioned(
          top: 12,
          left: 12,
          child: _DetectionInfoOverlay(),
        ),
        if (!_state.isStreaming)
          const Center(
            child: Chip(
              label: Text('Detection paused'),
              backgroundColor: Color(0xAA000000),
            ),
          ),
      ],
    );
  }

  Widget _buildControls() {
    final frame = _state.frame;
    return Container(
      decoration: const BoxDecoration(
        color: Color(0xFF1E1E1E),
        border: Border(top: BorderSide(color: Color(0xFF333333))),
      ),
      padding: const EdgeInsets.fromLTRB(12, 10, 12, 12),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          const Text(
            'AI VISION MODE',
            style: TextStyle(
              fontSize: 10,
              letterSpacing: 1.2,
              color: Color(0xFFA0A0A0),
            ),
          ),
          const SizedBox(height: 8),
          Row(
            children: [
              Expanded(
                child: FilledButton.icon(
                  style: FilledButton.styleFrom(
                    backgroundColor: _state.kind == DetectorKind.face
                        ? const Color(0xFF1E88E5)
                        : const Color(0xFF333333),
                  ),
                  icon: const Icon(Icons.face_outlined, size: 18),
                  label: const Text('Face'),
                  onPressed: () => unawaited(_state.setKind(DetectorKind.face)),
                ),
              ),
              const SizedBox(width: 8),
              Expanded(
                child: FilledButton.icon(
                  style: FilledButton.styleFrom(
                    backgroundColor: _state.kind == DetectorKind.object
                        ? const Color(0xFF1E88E5)
                        : const Color(0xFF333333),
                  ),
                  icon: const Icon(Icons.center_focus_strong_outlined, size: 18),
                  label: const Text('Object'),
                  onPressed: () => unawaited(_state.setKind(DetectorKind.object)),
                ),
              ),
            ],
          ),
          const SizedBox(height: 10),
          Row(
            children: [
              _StatusText('Tracking: ${_state.tracking.label}'),
              const SizedBox(width: 12),
              TextButton(
                onPressed: _state.tracking.status == TrackingStatus.none
                    ? null
                    : _state.clearTracking,
                child: const Text('Clear lock'),
              ),
            ],
          ),
          const Text(
            'Tap an object in the preview to lock tracking',
            style: TextStyle(fontSize: 10, color: Color(0xFF808080)),
          ),
          Wrap(
            spacing: 14,
            runSpacing: 4,
            children: [
              _StatusText('Model: ${_state.kind.modelName}'),
              _StatusText('Detections: ${frame?.count ?? 0}'),
              _StatusText('Inference: ${_state.fps}/s'),
              _StatusText(
                'Latency: ${frame?.inferenceTime.inMilliseconds ?? 0} ms',
              ),
            ],
          ),
        ],
      ),
    );
  }
}

/// CameraPreview rotates the texture itself; expose the resulting size so the
/// FittedBox can crop instead of stretch it.
Size _previewDisplaySize(CameraController controller) {
  final raw = controller.value.previewSize;
  if (raw == null) return const Size(720, 1280);
  final orientation = controller.value.deviceOrientation;
  final landscape = orientation == DeviceOrientation.landscapeLeft ||
      orientation == DeviceOrientation.landscapeRight;
  return landscape ? raw : Size(raw.height, raw.width);
}

class _StatusText extends StatelessWidget {
  const _StatusText(this.text);

  final String text;

  @override
  Widget build(BuildContext context) => Text(
        text,
        style: const TextStyle(fontSize: 11, color: Color(0xFFB5D4FF)),
      );
}

class _DetectionInfoOverlay extends StatelessWidget {
  const _DetectionInfoOverlay();

  @override
  Widget build(BuildContext context) {
    return IgnorePointer(
      child: Container(
        padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
        decoration: BoxDecoration(
          color: const Color(0x99000000),
          borderRadius: BorderRadius.circular(4),
        ),
        child: Text(
          DateTime.now().toString().substring(0, 19),
          style: const TextStyle(
            color: Color(0xFF00FF00),
            fontFamily: 'monospace',
            fontWeight: FontWeight.bold,
          ),
        ),
      ),
    );
  }
}
