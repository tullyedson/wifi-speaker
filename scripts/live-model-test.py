"""Verify local STT -> configured live model -> local TTS using an isolated simulated speaker."""
import argparse
import asyncio
import importlib.util
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import time
from types import SimpleNamespace
import urllib.request
import uuid
import wave


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--settings', type=Path, default=Path(os.environ['APPDATA']) / 'com.smartspeaker.desktop' / 'settings.json')
    parser.add_argument('--exe', type=Path, default=root / 'artifacts/Smart Speaker/speaker-hub.exe')
    args = parser.parse_args()
    settings = json.loads(args.settings.read_text(encoding='utf-8-sig'))
    if settings.get('backend_mode') != 'open_ai' or not settings.get('outbound_url') or not settings.get('model'):
        raise SystemExit('Configure the intended OpenAI-compatible model in the desktop app first.')
    if not Path(settings['whisper_model']).is_file():
        raise SystemExit('The configured local speech model is missing.')
    operation = uuid.uuid4().hex
    directory = root / 'runtime' / ('live-model-' + operation[:12])
    directory.mkdir(parents=True)
    with socket.socket() as reserved:
        reserved.bind(('127.0.0.1', 0))
        port = reserved.getsockname()[1]
    base = f'http://127.0.0.1:{port}'
    speaker_id = 'voice-check-' + operation
    settings.update(listen_url=base, public_url=base, api_token=secrets.token_hex(32), speakers=[dict(id=speaker_id, name='Voice check', tags=['verification'], token=secrets.token_hex(32), enabled=True)])
    wake = settings.get('wake_names', ['Speaker'])[0]
    private_config = directory / 'settings.json'
    report = dict(test='local-speech-live-model-roundtrip', success=False, model=settings['model'], simulated_speaker=True)
    process = None
    hidden = subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0
    started = time.monotonic()
    try:
        private_config.write_text(json.dumps(settings), encoding='utf-8')
        with (directory / 'hub.log').open('w', encoding='utf-8') as log:
            process = subprocess.Popen([str(args.exe), '--config', str(private_config)], stdout=log, stderr=log, creationflags=hidden)
            deadline = time.monotonic() + 15
            while True:
                if process.poll() is not None:
                    raise RuntimeError('The isolated hub exited during startup.')
                try:
                    with urllib.request.urlopen(base + '/healthz', timeout=1):
                        break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise RuntimeError('The isolated hub did not start.')
                    time.sleep(.1)
            input_wav, reply_wav = directory / 'input.wav', directory / 'reply.wav'
            subprocess.run([str(args.exe), '--tts', f'Please say that the speaker test passed, {wake}.', '--output', str(input_wav)], check=True, timeout=30, creationflags=hidden)
            spec = importlib.util.spec_from_file_location('speaker_simulator', root / 'scripts/simulate-speaker.py')
            simulator = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(simulator)
            try:
                asyncio.run(simulator.simulate(SimpleNamespace(config=private_config, hub=base, speaker=speaker_id, wav=input_wav, output=reply_wav, timeout=settings['response_timeout_seconds'] + 45)))
            except Exception as error:
                request = urllib.request.Request(base + '/v1/speakers', headers={'Authorization': 'Bearer ' + settings['api_token']})
                with urllib.request.urlopen(request, timeout=3) as response:
                    status = json.load(response)
                report['speaker_error'] = status['speakers'][0]['last_error'] or str(error) or type(error).__name__
                raise
            with wave.open(str(reply_wav), 'rb') as audio:
                if (audio.getframerate(), audio.getnchannels(), audio.getsampwidth()) != (16000, 1, 2):
                    raise RuntimeError('Returned speech is not 16 kHz mono PCM.')
                report['reply_seconds'] = round(audio.getnframes() / 16000, 3)
            spoken = subprocess.run([str(args.exe), '--transcribe', str(reply_wav), '--model', settings['whisper_model']], check=True, timeout=60, capture_output=True, encoding='utf-8', errors='replace', creationflags=hidden).stdout.strip()
            report['reply_transcript'] = spoken
            if not all(word in spoken.lower() for word in ('speaker', 'test', 'passed')):
                raise RuntimeError('The spoken response did not confirm the requested test phrase.')
            report.update(success=True, reply_file=str(reply_wav), elapsed_seconds=round(time.monotonic() - started, 3))
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=10)
        private_config.unlink(missing_ok=True)
        (directory / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        print(json.dumps(report, indent=2))
        print('Report:', directory / 'report.json')


if __name__ == '__main__':
    main()
