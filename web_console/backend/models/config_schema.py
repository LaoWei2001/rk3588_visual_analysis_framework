from __future__ import annotations
from typing import List, Optional, Union, Any
from pydantic import BaseModel, field_validator, model_serializer


class RTSPStream(BaseModel):
    url: str
    video_enc: str = "h264"

    def model_dump(self, **kwargs):
        return {"url": self.url, "video_enc": self.video_enc}


class USBStream(BaseModel):
    src_type: str = "usb"
    device: str

    def model_dump(self, **kwargs):
        return {"src_type": "usb", "device": self.device}


class FileStream(BaseModel):
    src_type: str = "file"
    url: str
    loop: bool = True

    def model_dump(self, **kwargs):
        return {"src_type": "file", "url": self.url, "loop": self.loop}


class GlobalLogicConfig(BaseModel):
    enable: bool = True
    logic: str = "global_default"
    channels: List[int] = []
    poll_interval_ms: int = 200


class GlobalConfig(BaseModel):
    model_path: str = ""
    label_path: str = ""
    enable_display: int = 0
    disp_width: int = 1920
    disp_height: int = 1080
    tile_rows: int = 2
    tile_cols: int = 2
    max_fps: int = 15
    queue_size: int = 1
    channel_threads: int = 1
    npu_cores: int = 3
    obj_thresh: float = 0.3
    nms_thresh: float = 0.45
    detect_classes: List[str] = []
    tracker_enable: int = 1
    tracker_iou_thresh: float = 0.3
    tracker_max_miss: int = 30
    tracker_min_hits: int = 3
    performance_display: int = 1
    enable_pause_key: int = 0
    playback_fps: Optional[int] = None
    debug_display: Optional[int] = None
    global_logics: Optional[List[GlobalLogicConfig]] = None

    def model_dump_clean(self) -> dict:
        d = self.model_dump(exclude_none=True)
        if not d.get("global_logics"):
            d.pop("global_logics", None)
        return d


class ChannelConfig(BaseModel):
    id: int
    enable: bool = True
    stream: Any
    npu_core: int = 0
    logic: str = "logic_default"
    model_type: str = "yolov8_det"
    model_path: str = ""
    label_path: str = ""
    obj_thresh: float = 0.3
    nms_thresh: float = 0.45
    detect_classes: List[str] = []
    threads: Optional[int] = None
    max_fps: Optional[int] = None
    playback_fps: Optional[int] = None
    tracker_enable: Optional[int] = None
    tracker_iou_thresh: Optional[float] = None
    tracker_max_miss: Optional[int] = None
    tracker_min_hits: Optional[int] = None
    dify_prompt: Optional[str] = None
    radius: Optional[int] = None
    version: Optional[str] = None

    def model_dump_clean(self) -> dict:
        d = {}
        d["id"] = self.id
        d["enable"] = self.enable
        # stream: pass through as-is (dict)
        if isinstance(self.stream, dict):
            d["stream"] = self.stream
        elif hasattr(self.stream, "model_dump"):
            d["stream"] = self.stream.model_dump()
        else:
            d["stream"] = self.stream
        d["npu_core"] = self.npu_core
        d["logic"] = self.logic
        d["model_type"] = self.model_type
        d["model_path"] = self.model_path
        d["label_path"] = self.label_path
        d["obj_thresh"] = self.obj_thresh
        d["nms_thresh"] = self.nms_thresh
        d["detect_classes"] = self.detect_classes
        # optional fields
        for field in ["threads", "max_fps", "playback_fps",
                      "tracker_enable", "tracker_iou_thresh",
                      "tracker_max_miss", "tracker_min_hits",
                      "dify_prompt", "radius", "version"]:
            val = getattr(self, field)
            if val is not None:
                d[field] = val
        return d


class AppConfig(BaseModel):
    schema_version: int = 2
    global_: GlobalConfig
    channels: List[ChannelConfig]

    model_config = {"populate_by_name": True}

    def model_dump_clean(self) -> dict:
        return {
            "schema_version": self.schema_version,
            "global": self.global_.model_dump_clean(),
            "channels": [ch.model_dump_clean() for ch in self.channels],
        }


class ROIZone(BaseModel):
    polygon: List[List[int]] = []


ROIZones = dict  # { "0": {"polygon": [[x,y],...]} }
