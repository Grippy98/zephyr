#!/usr/bin/env python3
"""Dependency-free local server that simulates the embedded dog-door API."""

from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import json
import os
import threading
import time
from urllib.parse import urlparse

ROOT = Path(__file__).resolve().parents[1]
WEB = ROOT / "web"
STARTED = time.monotonic()
LOCK = threading.Lock()
MOTION_TIMER = None
STATE = {
    "deviceName": "Mudroom Door",
    "homeTarget": "upper",
    "door": {"state": "closed", "upperLimit": False, "lowerLimit": True,
             "actuatorArmed": True, "motorReady": True, "fault": ""},
    "led": {"mode": "status", "red": 47, "green": 125, "blue": 74, "brightness": 70},
    "network": {"apActive": True, "connected": True, "connecting": False,
                "scanning": False, "ssid": "HomeNet", "ip": "192.168.1.82", "rssi": -48,
                "setupSsid": "DogDoor-Setup", "scan": [
                    {"ssid": "HomeNet", "rssi": -48, "channel": 6, "secure": True},
                    {"ssid": "Guest", "rssi": -67, "channel": 11, "secure": True},
                    {"ssid": "Workshop-IoT", "rssi": -76, "channel": 1, "secure": True},
                ]},
    "mqtt": {"enabled": True, "connected": True, "host": "homeassistant.local",
             "port": 1883, "username": "mqttuser"},
    "ota": {"supported": True, "imageConfirmed": True, "uploading": False,
            "rebootPending": False, "bytesReceived": 0, "runningVersion": "1.1.0",
            "updateVersion": ""},
}


def snapshot():
    with LOCK:
        value = json.loads(json.dumps(STATE))
    value["uptimeSeconds"] = int(time.monotonic() - STARTED)
    return value


def finish_motion(target):
    global MOTION_TIMER
    with LOCK:
        STATE["door"].update(state=target, upperLimit=target == "open",
                             lowerLimit=target == "closed")
        MOTION_TIMER = None


def finish_ota_boot():
    with LOCK:
        STATE["ota"].update(imageConfirmed=False, rebootPending=False,
                            runningVersion="1.1.0", updateVersion="")
    threading.Timer(3, confirm_ota).start()


def confirm_ota():
    with LOCK:
        STATE["ota"]["imageConfirmed"] = True


class Handler(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def translate_path(self, path):
        clean = urlparse(path).path.lstrip("/")
        return str(WEB / (clean or "index.html"))

    def send_json(self, payload, code=200):
        body = json.dumps(payload).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/api/state":
            self.send_json(snapshot())
        elif path == "/api/wifi/scan":
            with LOCK:
                STATE["network"]["scanning"] = False
            self.send_json(snapshot())
        elif path == "/setup":
            self.path = "/index.html"
            super().do_GET()
        else:
            super().do_GET()

    def read_json(self):
        return json.loads(self.rfile.read(int(self.headers.get("Content-Length", "0"))) or b"{}")

    def do_POST(self):
        global MOTION_TIMER
        path = urlparse(self.path).path

        if path == "/api/ota":
            length = int(self.headers.get("Content-Length", "0"))
            self.rfile.read(length)
            with LOCK:
                if not STATE["ota"]["imageConfirmed"]:
                    allowed = False
                else:
                    allowed = True
                    STATE["door"]["state"] = "stopped"
                    STATE["ota"].update(rebootPending=True, bytesReceived=length,
                                        updateVersion="1.1.0+0")
            if not allowed:
                self.send_json({"ok": False, "error": "Current firmware is still completing its rollback safety check"}, 409)
                return
            self.send_json({"ok": True, "message": "Firmware staged for test boot",
                            "version": "1.1.0+0", "bytesReceived": length,
                            "rebooting": True})
            threading.Timer(1.5, finish_ota_boot).start()
            return

        data = self.read_json()
        with LOCK:
            if path == "/api/door":
                command = data.get("command")
                if command in ("open", "close", "home"):
                    if MOTION_TIMER:
                        MOTION_TIMER.cancel()
                    target = "open" if command == "open" or command == "home" and STATE["homeTarget"] == "upper" else "closed"
                    STATE["door"].update(state="homing" if command == "home" else f"{command}ing",
                                         upperLimit=False, lowerLimit=False)
                    MOTION_TIMER = threading.Timer(1.1, finish_motion, [target])
                    MOTION_TIMER.start()
                elif command == "stop":
                    if MOTION_TIMER:
                        MOTION_TIMER.cancel()
                        MOTION_TIMER = None
                    STATE["door"]["state"] = "stopped"
            elif path == "/api/led":
                color = data.get("color", "#ffffff").lstrip("#")
                try: rgb = tuple(int(color[i:i + 2], 16) for i in (0, 2, 4))
                except ValueError: rgb = (255, 255, 255)
                STATE["led"].update(mode=data.get("mode", "status"), red=rgb[0], green=rgb[1],
                                    blue=rgb[2], brightness=int(data.get("brightness", 70)))
            elif path == "/api/config":
                if "deviceName" in data:
                    STATE["deviceName"] = data["deviceName"]
                if data.get("homeTarget") in ("upper", "lower"):
                    STATE["homeTarget"] = data["homeTarget"]
                mqtt_values = {}
                if "mqttEnabled" in data: mqtt_values["enabled"] = bool(data["mqttEnabled"])
                if "mqttHost" in data: mqtt_values["host"] = data["mqttHost"]
                if "mqttPort" in data: mqtt_values["port"] = int(data["mqttPort"])
                if "mqttUsername" in data: mqtt_values["username"] = data["mqttUsername"]
                STATE["mqtt"].update(**mqtt_values)
            elif path == "/api/wifi":
                STATE["network"].update(ssid=data.get("ssid", ""), connecting=True, connected=False)
                threading.Timer(1.2, self.connect_wifi).start()
            else:
                self.send_json({"ok": False, "error": "Not found"}, 404)
                return
        self.send_json(snapshot())

    @staticmethod
    def connect_wifi():
        with LOCK:
            STATE["network"].update(connecting=False, connected=True, ip="192.168.1.82")

    def log_message(self, fmt, *args):
        print(f"[dog-door-ui] {fmt % args}")


if __name__ == "__main__":
    os.chdir(WEB)
    server = ThreadingHTTPServer(("127.0.0.1", 8080), Handler)
    print("Dog Door UI: http://127.0.0.1:8080 (setup: /setup)")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        server.server_close()
