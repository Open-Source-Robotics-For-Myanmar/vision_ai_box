# RetinaFace face detection

`RetinaFacePlugin.cpp` runs a RetinaFace (ResNet50 backbone) ONNX model with
ONNX Runtime **CPU** and emits normalized face boxes in the project's
`PluginResult` format. No Python is used at build or test time.

## Model

`models/retinaface.onnx` is a ready-made ONNX export (FP16 weights, FP32 I/O)
of `biubug6/Pytorch_Retinaface` `Resnet50_Final.pth`, published by
`leandroveronezi/go-onnxface`:

```
curl -fL -o models/retinaface.onnx \
  https://github.com/leandroveronezi/go-onnxface/releases/download/models-v1/retinaface.onnx
```

Model files are excluded by `.gitignore`. Contract: input `input0` FP32
`[1,3,640,640]`; outputs `loc` `[1,N,4]`, `conf` `[1,N,2]`, `landms`
`[1,N,10]`, with `N = 16800` priors (strides 8/16/32, min sizes
`{16,32}/{64,128}/{256,512}`). Preprocessing: letterbox into the top-left of
a zero-padded 640x640 canvas, BGR pixels minus `(104,117,123)`, no `/255`,
no channel swap. Decoding uses variances `(0.1, 0.2)`, face score `conf[1]`,
greedy NMS at IoU `0.4` and maps boxes back through the letterbox scale.

Source: https://github.com/leandroveronezi/go-onnxface (MIT), upstream model
https://github.com/biubug6/Pytorch_Retinaface.

## Build and smoke test

```sh
cmake -S . -B build-usb -DCMAKE_BUILD_TYPE=Release -DCAMERA=USB \
  -DVISION_BUILD_FACE_DETECTION=ON -DONNXRUNTIME_ROOT=/absolute/path/to/onnxruntime
cmake --build build-usb -j2
./build-usb/bin/face_detection_smoke \
  plugins/face_detection/models/retinaface.onnx /absolute/path/to/photo.jpg
```

The smoke binary loads the model, runs one frame, validates normalized boxes,
checks that a disabled plugin emits nothing, and prints JSON. No face found
is not an error. `VISION_FACE_MODEL` overrides the default model path.
