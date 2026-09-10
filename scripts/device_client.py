"""Small reference client for the device API. No speech or LLM service is required."""
import argparse
import json
import os
from pathlib import Path
import re
import urllib.error
import urllib.parse
import urllib.request


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, file, code, message, headers, new_url):
        return None


class DeviceError(Exception):
    def __init__(self, status: int):
        self.status = status
        super().__init__(f"Device returned HTTP {status}; consult docs/device-api.md")


class DeviceClient:
    def __init__(self, url: str, token: str, timeout: float = 10):
        parsed = urllib.parse.urlsplit(url)
        if (parsed.scheme != "http" or not parsed.hostname or parsed.username or
                parsed.password or parsed.query or parsed.fragment or parsed.path not in ("", "/")):
            raise ValueError("Use the device's HTTP origin without credentials, path or query")
        if not re.fullmatch(r"[A-Za-z0-9_-]{32,256}", token):
            raise ValueError("A device control token is required")
        self.url = url.rstrip("/")
        self.token = token
        self.timeout = timeout
        self.opener = urllib.request.build_opener(NoRedirect)

    def request(self, method: str, path: str, body: dict | None = None) -> dict:
        if path not in ("/v1/device", "/v1/config", "/v1/display", "/v1/led", "/v1/alarms", "/v1/alarms/stop"):
            raise ValueError("Unknown device endpoint")
        data = None if body is None else json.dumps(body, allow_nan=False).encode("utf-8")
        if data is not None and len(data) > 4096:
            raise ValueError("Device JSON is limited to 4096 bytes")
        request = urllib.request.Request(self.url + path, data=data, method=method,
                                         headers={"Authorization": "Bearer " + self.token,
                                                  "Content-Type": "application/json"})
        try:
            with self.opener.open(request, timeout=self.timeout) as response:
                raw = response.read(65537)
        except urllib.error.HTTPError as error:
            raise DeviceError(error.code) from None
        if len(raw) > 65536:
            raise ValueError("Device response exceeds the reference client's limit")
        result = json.loads(raw)
        if not isinstance(result, dict):
            raise ValueError("Expected a JSON object from the device")
        return result

    def status(self) -> dict:
        return self.request("GET", "/v1/device")

    def config(self) -> dict:
        return self.request("GET", "/v1/config")

    def configure(self, **settings) -> dict:
        return self.request("PATCH", "/v1/config", settings)

    def display(self, **command) -> dict:
        return self.request("POST", "/v1/display", command)

    def led(self, **command) -> dict:
        return self.request("POST", "/v1/led", command)

    def alarm(self, alarm_id: str, duration_ms: int, pattern: str = "chime", volume: int = 50) -> dict:
        return self.request("POST", "/v1/alarms", {"id": alarm_id, "duration_ms": duration_ms,
                                                   "pattern": pattern, "volume": volume})

    def stop_alarm(self, alarm_id: str) -> dict:
        return self.request("POST", "/v1/alarms/stop", {"id": alarm_id})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True)
    parser.add_argument("--token-env", default="SPEAKER_CONTROL_TOKEN", help="Environment variable containing the private control token")
    parser.add_argument("--provision", type=Path, help="Alternatively read control_token (or speaker_token) from a private provisioning file")
    parser.add_argument("action", choices=("status", "config", "configure", "display", "led", "alarm", "stop-alarm"))
    parser.add_argument("--body", type=Path, help="Private JSON file for write operations")
    args = parser.parse_args()
    token = os.environ.get(args.token_env, "")
    if args.provision:
        provision = json.loads(args.provision.read_text(encoding="utf-8-sig"))
        token = provision.get("control_token", provision.get("speaker_token", ""))
    client = DeviceClient(args.url, token)
    paths = {"status": ("GET", "/v1/device"), "config": ("GET", "/v1/config"),
             "configure": ("PATCH", "/v1/config"), "display": ("POST", "/v1/display"),
             "led": ("POST", "/v1/led"), "alarm": ("POST", "/v1/alarms"),
             "stop-alarm": ("POST", "/v1/alarms/stop")}
    method, path = paths[args.action]
    if method != "GET" and not args.body:
        parser.error("Write operations require --body with a JSON file")
    body = json.loads(args.body.read_text(encoding="utf-8-sig")) if args.body else None
    print(json.dumps(client.request(method, path, body), indent=2))


if __name__ == "__main__":
    main()
