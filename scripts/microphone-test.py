"""Capture one explicit, bounded microphone sample and inspect it locally."""
import argparse
from array import array
from concurrent.futures import ThreadPoolExecutor
import json
import math
import os
from pathlib import Path
import re
import statistics
import subprocess
import tempfile
import time
import urllib.request
import urllib.parse
import uuid
import wave


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--speaker', required=True)
    parser.add_argument('--seconds', type=int, default=15)
    parser.add_argument('--settings', type=Path, default=Path(os.environ['APPDATA']) / 'com.smartspeaker.desktop/settings.json')
    parser.add_argument('--observe', action='store_true', help='Inspect the next normal live transcription without suspending prompt routing')
    parser.add_argument('--guide', action='store_true', help='Have the selected speaker give an audible cue before capture')
    parser.add_argument('--play', type=Path, help='Play an optional test WAV through this PC after capture starts')
    parser.add_argument('--output', type=Path, help='Explicitly retain the test WAV; otherwise it is removed after analysis')
    args = parser.parse_args()
    limit = 60 if args.observe else 15
    if not 1 <= args.seconds <= limit:
        parser.error(f"--seconds must be between 1 and {limit}")
    if args.observe and args.output:
        parser.error("--output only applies to WAV sample checks")
    settings = json.loads(args.settings.read_text(encoding='utf-8-sig'))
    if not any(s['id'] == args.speaker for s in settings['speakers']):
        raise SystemExit('Speaker is not in the supplied settings.')
    listen = urllib.parse.urlsplit(settings['listen_url'])
    host = '127.0.0.1' if listen.hostname in ('0.0.0.0', '::') else listen.hostname
    base = 'http://' + str(host) + ':' + str(listen.port or 80)
    headers = {'Authorization': 'Bearer ' + settings['api_token']}
    if args.guide:
        cue = {'request_id':str(uuid.uuid4()),'speaker_ids':[args.speaker],'tags':[],'text':f"Microphone test. After I finish, wait two seconds. Then say: {settings['wake_names'][0]}, please tell me the time. Say it twice."}
        request = urllib.request.Request(base + '/v1/speak',data=json.dumps(cue).encode(),headers={**headers,'Content-Type':'application/json'})
        with urllib.request.urlopen(request,timeout=10): pass
        deadline = time.monotonic() + 60
        saw_playback = False
        while time.monotonic() < deadline:
            request = urllib.request.Request(base + '/v1/speakers',headers=headers)
            with urllib.request.urlopen(request,timeout=3) as response: status=json.load(response)
            selected = next(s for s in status['speakers'] if s['speaker']['id']==args.speaker)
            if selected['last_error']: raise RuntimeError(selected['last_error'])
            saw_playback = saw_playback or selected['phase'] == 'speaking'
            if saw_playback and selected['phase'] == 'listening': break
            time.sleep(.1)
        else:
            raise RuntimeError('The microphone cue did not finish playing.')
    def capture():
        request = urllib.request.Request(base + '/v1/speakers/' + args.speaker + ('/recognition-test' if args.observe else '/microphone-sample'), data=json.dumps({'seconds':args.seconds}).encode(), headers={**headers,'Content-Type':'application/json'})
        with urllib.request.urlopen(request, timeout=args.seconds + 8) as response:
            data = response.read(480_045)
        if len(data) > 480_044:
            raise RuntimeError('Microphone sample exceeded the bounded WAV size.')
        return data
    with ThreadPoolExecutor(max_workers=1) as executor:
        pending = executor.submit(capture)
        deadline = time.monotonic() + 5
        while not pending.done():
            request = urllib.request.Request(base + '/v1/speakers', headers=headers)
            with urllib.request.urlopen(request, timeout=3) as response:
                status = json.load(response)
            selected = next(s for s in status['speakers'] if s['speaker']['id'] == args.speaker)
            if selected['recognition_test_active' if args.observe else 'microphone_test_active']:
                print('Microphone check active. Say your test phrase now.', flush=True)
                if args.play:
                    import winsound
                    winsound.PlaySound(str(args.play.resolve()), winsound.SND_FILENAME | winsound.SND_ASYNC)
                break
            if time.monotonic() >= deadline:
                raise RuntimeError('Microphone check did not become active.')
            time.sleep(.05)
        audio = pending.result()
    if args.observe:
        print(json.dumps(json.loads(audio),indent=2),flush=True)
        return
    with tempfile.TemporaryDirectory(prefix='speaker-mic-check-') as temporary:
        sample_path = Path(temporary) / 'sample.wav'
        sample_path.write_bytes(audio)
        with wave.open(str(sample_path), 'rb') as recording:
            if (recording.getframerate(), recording.getnchannels(), recording.getsampwidth()) != (16000, 1, 2):
                raise RuntimeError('Unexpected microphone sample format.')
            samples = array('h', recording.readframes(recording.getnframes()))
        levels = [math.sqrt(sum((v / 32768) ** 2 for v in samples[i:i+320])/len(samples[i:i+320])) for i in range(0,len(samples),320)]
        result = subprocess.run([str(root/'artifacts/Smart Speaker/speaker-hub.exe'),'--transcribe',str(sample_path),'--model',settings['whisper_model']], check=True, timeout=60, capture_output=True, encoding='utf-8', errors='replace', creationflags=subprocess.CREATE_NO_WINDOW)
        text = result.stdout.strip()
        words = re.findall(r'\w+', text.lower())
        report = dict(speaker_id=args.speaker, seconds=len(samples)/16000, peak=max(abs(x) for x in samples)/32768, rms=math.sqrt(sum((x/32768)**2 for x in samples)/len(samples)), mean=statistics.mean(samples)/32768, frame_rms_median=statistics.median(levels), frame_rms_max=max(levels), frames_above_threshold=sum(x >= settings['speech_threshold'] for x in levels), clipped_samples=sum(abs(x)>=32767 for x in samples), transcript=text, wake_name=next((name for name in settings['wake_names'] if any(words[i:i+len(name.lower().split())] == name.lower().split() for i in range(len(words)))),None))
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_bytes(audio)
            report['saved_audio'] = str(args.output.resolve())
        print(json.dumps(report, indent=2), flush=True)


if __name__ == '__main__':
    main()
