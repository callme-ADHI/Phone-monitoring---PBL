"""
person_counter.py — Real-time person counting via YOLOv8n + OpenCV
with local HTTP API server for dashboard integration.
"""

import argparse
import collections
import http.server
import json
import statistics
import threading
import time

import cv2
import numpy as np
import requests
from ultralytics import YOLO

MODEL_FILE       = "yolov8n.pt"
CONF_THRESHOLD   = 0.40
PERSON_CLASS_ID  = 0
SMOOTH_WINDOW    = 5
FPS_REPORT_SECS  = 1.0

BOX_COLOR        = (0, 255, 0)
BOX_THICKNESS    = 2
LABEL_FONT       = cv2.FONT_HERSHEY_SIMPLEX
LABEL_SCALE      = 0.55
LABEL_THICKNESS  = 1
COUNT_FONT_SCALE = 1.4
COUNT_THICKNESS  = 3
COUNT_COLOR      = (0, 255, 255)

# Thread-safe shared state for live count and history
live_state = {
    "personCount": 0,
    "rawCount": 0,
    "lastUpdated": time.time(),
    "history": []  # List of {"personCount": int, "timestamp": str}
}
state_lock = threading.Lock()


class WebcamSource:
    def __init__(self, index: int = 0):
        self._cap = cv2.VideoCapture(index)
        if not self._cap.isOpened():
            raise RuntimeError(
                f"Cannot open camera at index {index}. "
                "Try --camera with a different index, or check no other app is using it."
            )

    def read(self) -> tuple[bool, cv2.typing.MatLike]:
        return self._cap.read()

    def release(self) -> None:
        self._cap.release()


class ESP32CamSource:
    """Pulls single JPEG frames from an ESP32-CAM's /capture endpoint.
    Used instead of a local webcam once the camera module is working."""

    def __init__(self, base_url: str, timeout: float = 4.0):
        self._base = base_url.rstrip("/")
        self._timeout = timeout
        self._session = requests.Session()
        # fail fast if the camera isn't reachable at all
        self.read()

    def read(self) -> tuple[bool, "cv2.typing.MatLike | None"]:
        try:
            r = self._session.get(f"{self._base}/capture", timeout=self._timeout)
            r.raise_for_status()
            frame = cv2.imdecode(np.frombuffer(r.content, np.uint8), cv2.IMREAD_COLOR)
            if frame is None:
                return False, None
            return True, frame
        except requests.RequestException:
            return False, None

    def release(self) -> None:
        self._session.close()


def detect_people(model: YOLO, frame, conf: float) -> list[dict]:
    results = model(frame, classes=[PERSON_CLASS_ID], conf=conf, verbose=False)
    detections = []
    for box in results[0].boxes:
        x1, y1, x2, y2 = map(int, box.xyxy[0].tolist())
        detections.append({"bbox": (x1, y1, x2, y2), "conf": float(box.conf[0])})
    return detections


def draw_detections(frame, detections: list[dict]) -> None:
    for det in detections:
        x1, y1, x2, y2 = det["bbox"]
        cv2.rectangle(frame, (x1, y1), (x2, y2), BOX_COLOR, BOX_THICKNESS)
        cv2.putText(
            frame, f"{det['conf']:.2f}",
            (x1, max(y1 - 6, 14)),
            LABEL_FONT, LABEL_SCALE, BOX_COLOR, LABEL_THICKNESS, cv2.LINE_AA,
        )


def draw_count(frame, count: int) -> None:
    cv2.putText(
        frame, f"People: {count}", (16, 48),
        LABEL_FONT, COUNT_FONT_SCALE, COUNT_COLOR, COUNT_THICKNESS, cv2.LINE_AA,
    )


class OccupancyAPIHandler(http.server.BaseHTTPRequestHandler):
    def do_OPTIONS(self):
        self.send_response(200)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()

    def do_GET(self):
        if self.path == "/api/occupancy" or self.path == "/":
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.end_headers()
            with state_lock:
                data = {
                    "personCount": live_state["personCount"],
                    "rawCount": live_state["rawCount"],
                    "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(live_state["lastUpdated"])),
                    "history": live_state["history"][-72:]
                }
            self.wfile.write(json.dumps(data).encode("utf-8"))
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, format, *args):
        # Silence HTTP request logging in console to avoid cluttering FPS output
        pass


def start_api_server(port: int = 8000):
    server = http.server.ThreadingHTTPServer(("0.0.0.0", port), OccupancyAPIHandler)
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()
    print(f"Occupancy API server live at http://localhost:{port}/api/occupancy")
    return server


def run(camera_index: int, conf: float, port: int, source_kind: str, cam_url: str) -> None:
    model = YOLO(MODEL_FILE)
    if source_kind == "esp32cam":
        print(f"Pulling frames from ESP32-CAM at {cam_url}")
        source = ESP32CamSource(cam_url)
    else:
        print(f"Opening system webcam index {camera_index}")
        source = WebcamSource(index=camera_index)
    start_api_server(port)

    count_history: collections.deque[int] = collections.deque(maxlen=SMOOTH_WINDOW)
    fps_frames = 0
    fps_t0 = time.perf_counter()
    last_history_sample_time = 0

    print("Running -- press 'q' in the video window to quit.")
    try:
        while True:
            ok, frame = source.read()
            if not ok or frame is None:
                print("Warning: failed to read frame; retrying.")
                time.sleep(0.2)
                continue

            detections = detect_people(model, frame, conf)
            raw_c = len(detections)
            count_history.append(raw_c)
            smoothed = int(statistics.median(count_history))

            now = time.time()
            iso_now = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now))

            with state_lock:
                live_state["personCount"] = smoothed
                live_state["rawCount"] = raw_c
                live_state["lastUpdated"] = now
                # Record a history point every 2 seconds for the sparkline chart
                if now - last_history_sample_time >= 2.0:
                    live_state["history"].append({
                        "personCount": smoothed,
                        "timestamp": iso_now
                    })
                    if len(live_state["history"]) > 72:
                        live_state["history"].pop(0)
                    last_history_sample_time = now

            draw_detections(frame, detections)
            draw_count(frame, smoothed)
            cv2.imshow("Person Counter  [q to quit]", frame)

            fps_frames += 1
            elapsed = time.perf_counter() - fps_t0
            if elapsed >= FPS_REPORT_SECS:
                print(f"FPS: {fps_frames / elapsed:.1f}  |  People: {smoothed}")
                fps_frames = 0
                fps_t0 = time.perf_counter()

            if cv2.waitKey(1) & 0xFF == ord("q"):
                break

    except KeyboardInterrupt:
        print("\nInterrupted.")
    finally:
        source.release()
        cv2.destroyAllWindows()
        print("Camera released. Exiting.")


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Real-time person counter using YOLOv8n.")
    p.add_argument("--camera", type=int, default=0,
                   help="OpenCV camera device index (default: 0).")
    p.add_argument("--conf", type=float, default=CONF_THRESHOLD,
                   help=f"Confidence threshold 0-1 (default: {CONF_THRESHOLD}).")
    p.add_argument("--port", type=int, default=8000,
                   help="Port for the HTTP API (default: 8000).")
    p.add_argument("--source", choices=["webcam", "esp32cam"], default="webcam",
                   help="Frame source: system webcam (default) or ESP32-CAM.")
    p.add_argument("--cam-url", default="http://192.168.4.1",
                   help="Base URL of the ESP32-CAM when --source esp32cam (default: http://192.168.4.1).")
    return p.parse_args()


if __name__ == "__main__":
    args = parse_args()
    run(camera_index=args.camera, conf=args.conf, port=args.port,
        source_kind=args.source, cam_url=args.cam_url)