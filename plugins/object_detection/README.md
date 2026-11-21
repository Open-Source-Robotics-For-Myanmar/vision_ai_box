# YOLO26 Large object detection

`Yolo26Plugin.cpp` သည် YOLO26 Large ONNX model ကို ONNX Runtime **CPU** ဖြင့် run ပြီး dashboard overlay အတွက် normalized bounding boxes ထုတ်ပေးသည်။ Pipeline preprocessing ရှိလျှင် ထို stage မှထွက်လာသော frame ကို လက်ခံသည်။ `.pt` ကို C++ မှ တိုက်ရိုက်မဖတ်ပါ။

## Model download/export

Repository root မှ run ပါ။ ပထမ download အတွက် internet လိုသည်။

```sh
python3 -m venv plugins/object_detection/.venv
. plugins/object_detection/.venv/bin/activate
python -m pip install --upgrade ultralytics onnx onnxruntime
python plugins/object_detection/export_model.py
```

Default output: `plugins/object_detection/models/yolo26l.onnx` နှင့် `yolo26l.names.json`။ Model weights ကို Git မှ exclude လုပ်ထားသည်။ ကိုယ်ပိုင် YOLO26 detection checkpoint အတွက် `--weights /absolute/path/best.pt` သုံးနိုင်ပြီး script ထုတ်ပေးသော model/labels paths ကို အောက်ပါ environment variables တွင် ထည့်ပါ။ ONNX external data ရှိလျှင် model နဲ့အတူ ထားပါ။

လက်ခံသော contract: static FP32 `[1,3,H,W]` input နှင့် end-to-end FP32 `[1,N,6]` output (`xyxy, confidence, class_id`)။ Letterbox padding 114၊ BGR→RGB၊ `/255` preprocessing နှင့် coordinate conversion ပါသည်။ NMS ထပ်မလုပ်ပါ။ Dynamic၊ FP16၊ raw one-to-many၊ segmentation/pose exports မထောက်ပံ့ပါ။

## Build

Project dependencies အပြင် architecture ကိုက်ညီသည့် [ONNX Runtime C/C++ SDK](https://github.com/microsoft/onnxruntime/releases) ကို download/extract လုပ်ထားပါ။ Python package တစ်ခုတည်းသည် C++ SDK အစား မဖြစ်ပါ။

```sh
cmake -S . -B build-usb -DCMAKE_BUILD_TYPE=Release -DCAMERA=USB \
  -DVISION_BUILD_YOLO26=ON -DONNXRUNTIME_ROOT=/absolute/path/to/onnxruntime
cmake --build build-usb -j2
```

`build-usb/bin/plugins/object_detection.so` ထွက်လာမည်။ Existing build များ SDK မလိုစေရန် option default `OFF` ဖြစ်သည်။ SDK libraries ကို runtime တွင် ဆက်ရရှိရန်ထားပါ။ RealSense အတွက် မူလ camera build options ကို သုံးနိုင်သည်။

## Camera မလိုဘဲ စမ်းရန်

```sh
./build-usb/bin/yolo26_smoke \
  plugins/object_detection/models/yolo26l.onnx \
  plugins/object_detection/models/yolo26l.names.json /absolute/path/test-image.jpg
```

Load/inference၊ box bounds နှင့် disabled behavior စစ်ပြီး JSON ထုတ်သည်။ Object မတွေ့ခြင်းသည် error မဟုတ်ပါ။ Accuracy ကို သီးခြားစစ်ရန်လိုသည်။

## Application မှ run ရန်

```sh
export VISION_YOLO26_MODEL="$PWD/plugins/object_detection/models/yolo26l.onnx"
export VISION_YOLO26_LABELS="$PWD/plugins/object_detection/models/yolo26l.names.json"
./build-usb/bin/vision_ai_box
```

Dashboard → Plugins → `object_detection` ရွေးပါ။ Confidence threshold (default `0.25`) နှင့် show labels ကို settings UI မှပြင်နိုင်သည်။ Model path/reload ကို active client token ပါသော `POST /api/plugins/settings` ဖြင့်ပြင်နိုင်သည်:

```json
{
  "name": "object_detection",
  "settings": {
    "model_path": "/absolute/path/yolo26l.onnx",
    "labels_path": "/absolute/path/yolo26l.names.json",
    "confidence_threshold": 0.25,
    "reload": true
  }
}
```

`POST /api/client` မှ token ယူပြီး `X-Vision-Client` header ဖြင့်ပို့ပါ။ Claim အသစ်လုပ်တိုင်း အရင် client session ပိတ်မည်။ `GET /api/plugins` ရှိ `settings.ready`/`last_error` နှင့် logs ကို စစ်ပါ။ Load/inference failure တွင် result အလွတ်ပြန်ပေးသည်။ ပြင်ပြီး Save Settings/reload သို့မဟုတ် unload/select လုပ်နိုင်သည်။ Settings သည် in-memory ဖြစ်ပြီး loading/inference ကြောင့် API call ခေတ္တစောင့်ရနိုင်သည်။

CUDA/TensorRT acceleration မပါသေးပါ။ Large model CPU FPS သည် hardware ပေါ်မူတည်သည်။ ဒီ environment တွင် OpenCV/ONNX Runtime development dependencies နှင့် weights မရှိသဖြင့် build/real-model inference မအတည်ပြုရသေးပါ။

ကိုးကား: [YOLO26](https://docs.ultralytics.com/models/yolo26/), [Export contract](https://docs.ultralytics.com/modes/export/), [ONNX Runtime C++](https://onnxruntime.ai/docs/api/c/struct_ort_1_1_session.html)။
