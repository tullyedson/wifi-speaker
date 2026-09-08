"""Send a private provisioning file over USB. Never print credentials."""
import argparse, json, time
from pathlib import Path
import serial

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--config', required=True, type=Path)
    args = parser.parse_args()
    config = json.loads(args.config.read_text(encoding='utf-8-sig'))
    required = ('wifi_ssid', 'wifi_password', 'hub_url', 'speaker_id', 'speaker_token')
    if any(not isinstance(config.get(key), str) for key in required):
        raise SystemExit('Provisioning JSON is missing a required string field. Copy it from speaker setup details.')
    config['type'] = 'provision'
    message = json.dumps(config, ensure_ascii=True, separators=(',', ':')).encode('utf-8') + b'\n'
    if len(message) > 2048:
        raise SystemExit('Provisioning message exceeds the firmware limit.')
    port = serial.Serial(port=None, baudrate=115200, timeout=0.5, write_timeout=3)
    port.port = args.port; port.dtr = False; port.rts = False
    with port:
        port.reset_input_buffer(); port.write(message); port.flush()
        deadline = time.monotonic() + 12
        while time.monotonic() < deadline:
            line = port.readline().strip()
            if line == b'PROVISIONED':
                print('Speaker accepted the settings and is restarting.'); return
            if line == b'INVALID_CONFIGURATION':
                raise SystemExit('Speaker rejected the settings. Check the Wi-Fi, hub URL, ID and token.')
    raise SystemExit('No provisioning acknowledgement. Check the port, power switch, and installed firmware.')
if __name__ == '__main__':
    main()
