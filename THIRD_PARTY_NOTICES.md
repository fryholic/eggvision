# Third-party components

This repository links to or is deployed with independently licensed components:

- libcamera
- GStreamer and gst-rtsp-server
- OpenCV
- Intel OpenVINO Runtime
- MNN 3.6.1, statically linked from source commit
  `d407447ed56c4121a11ccbd266dc184ca1ead0c2` under Apache License 2.0
- Raspberry Pi kernel/firmware camera and V4L2 codec drivers
- MediaMTX binary and configuration retained from the earlier prototype
- YOLOv5n OpenVINO model supplied separately by the operator

The untracked `LICENSE` next to the retained MediaMTX binary identifies MediaMTX's license and must not be interpreted as model licensing. Confirm all component and model terms before distributing a device image or repository release.

The MNN license text and the upstream license files shipped for its FlatBuffers,
half, protobuf, and RapidJSON dependencies are retained under `third_party/`.
A binary or device image containing the statically linked MNN runtime must carry
those licenses and this notice. Inclusion is conservative: a minimal runtime
build may not link code from every listed dependency. The separately supplied
YOLOv5n model is not covered by MNN's license.

