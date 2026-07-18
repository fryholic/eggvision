# Model assets

Place `yolov5n.xml` and `yolov5n.bin` in this directory. The verified model has:

- input: FP32 `[1,3,320,320]`, RGB, normalized to `[0,1]`
- output: FP32 `[1,6300,85]`
- class filter: COCO class 0 (`person`)

Model binaries are deliberately ignored by Git. Before redistribution, record the original checkpoint, exporter version, training dataset terms, and applicable Ultralytics/YOLOv5 license. The application source license does not automatically grant rights to a model file.

