#!/usr/bin/env python3
"""Download YOLO26 Large and export the C++ plugin's static FP32 ONNX contract."""
import argparse
import json
import os
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--weights', default='yolo26l.pt')
    parser.add_argument('--output', type=Path, default=Path(__file__).parent / 'models')
    parser.add_argument('--imgsz', type=int, default=640)
    args = parser.parse_args()
    if not 32 <= args.imgsz <= 4096 or args.imgsz % 32:
        parser.error('--imgsz must be a multiple of 32 between 32 and 4096')
    weights = Path(args.weights).expanduser()
    if weights.is_file():
        weights = weights.resolve()
    elif args.weights != 'yolo26l.pt':
        parser.error('Custom --weights must be an existing local checkpoint')
    from ultralytics import YOLO
    import onnx
    output = args.output.expanduser().resolve()
    output.mkdir(parents=True, exist_ok=True)
    os.chdir(output)
    model = YOLO(str(weights))
    if model.task != 'detect':
        raise ValueError('YOLO26 detection checkpoint required')
    exported = Path(model.export(format='onnx', imgsz=args.imgsz, batch=1,
        dynamic=False, half=False, device='cpu', end2end=True, nms=False,
        opset=17, simplify=False)).resolve()
    graph = onnx.load(str(exported))
    onnx.checker.check_model(graph)
    if len(graph.graph.input) != 1 or len(graph.graph.output) != 1:
        raise ValueError('Expected single input/output')
    inp, out = graph.graph.input[0].type.tensor_type, graph.graph.output[0].type.tensor_type
    shape = [dim.dim_value for dim in out.shape.dim]
    if inp.elem_type != onnx.TensorProto.FLOAT or [d.dim_value for d in inp.shape.dim] != [1, 3, args.imgsz, args.imgsz]:
        raise ValueError('Expected static FP32 input')
    if out.elem_type != onnx.TensorProto.FLOAT or len(shape) != 3 or shape[0] != 1 or shape[1] < 1 or shape[2] != 6:
        raise ValueError(f'Expected end-to-end [1,N,6] output, got {shape}')
    # Preserve any ONNX external data alongside the exported graph.
    labels = exported.with_suffix('.names.json')
    labels.write_text(json.dumps([model.names[i] for i in range(len(model.names))],
        ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(f'Model: {exported}\nLabels: {labels}')
    print('Set VISION_YOLO26_MODEL and VISION_YOLO26_LABELS to these paths.')


if __name__ == '__main__':
    main()
