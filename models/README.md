# Model assets

Place the MNN production model and OpenVINO rollback model in this directory:

```text
yolov5n.mnn
yolov5n.xml
yolov5n.bin
```

The verified models share this contract:

- input: FP32 `[1,3,320,320]`, RGB, normalized to `[0,1]`
- output: FP32 `[1,6300,85]`
- class filter: COCO class 0 (`person`)

The selected MNN FP32 model has SHA-256
`4f8276abc13ac915d2ecc59a9a16c857af84be697a1fe0c812fcfce33599cbc8`.
It was converted with MNN 3.6.1 from the reproduced YOLOv5n v7 ONNX opset 17
model. Startup rejects a model whose shape, dtype, or NCHW layout differs from
the contract. The MNN fingerprint is the model file SHA-256; the OpenVINO
fingerprint records both the XML and sibling BIN SHA-256 so a weights-only
change is observable.

OpenVINO remains an explicit rollback backend. Its verified XML and BIN hashes
are respectively `181ac01d646f660899b7866eefcfddc87307f5f0e87be741b4797636458d634e`
and `1348c3a0244992ed04d4713dea9a340aebfeacf027f4cc9df5242189a7348085`.

Model binaries are deliberately ignored by Git. Before redistribution, record the original checkpoint, exporter version, training dataset terms, and applicable Ultralytics/YOLOv5 license. The application source license does not automatically grant rights to a model file.

