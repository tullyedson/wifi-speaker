"""Exercise real local TTS -> simulated speaker -> STT -> callback app -> TTS -> speaker WAV."""
import argparse, asyncio, importlib.util, json, secrets, subprocess, threading, time, urllib.request, uuid, wave
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from types import SimpleNamespace
import socket

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, default=Path('target/release/speaker-hub.exe'))
    parser.add_argument('--model', type=Path, default=Path('models/ggml-base.en.bin'))
    args = parser.parse_args()
    directory = Path('runtime') / ('smoke-' + uuid.uuid4().hex[:12]); directory.mkdir(parents=True)
    report = {'test': 'real-local-speech-callback-roundtrip', 'success': False}
    received = []; failures = []
    app_token = secrets.token_hex(32)
    with socket.socket() as reserved:
        reserved.bind(('127.0.0.1',0)); hub_port = reserved.getsockname()[1]
    base = f'http://127.0.0.1:{hub_port}'
    class App(BaseHTTPRequestHandler):
        def log_message(self, *_): pass
        def do_POST(self):
            body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            received.append(body)
            self.send_response(202); self.end_headers()
            def respond():
                try:
                    payload = json.dumps({'request_id':body['request_id'],'speaker_id':body['speaker']['id'],'text':'Your speaker and local speech hub are working together.'}).encode()
                    request = urllib.request.Request(body['response_url'],data=payload,headers={'Content-Type':'application/json','Authorization':'Bearer '+app_token},method='POST')
                    with urllib.request.urlopen(request,timeout=10) as response:
                        if response.status != 202: raise RuntimeError('Callback was not accepted')
                except Exception as error: failures.append(str(error))
            threading.Thread(target=respond,daemon=True).start()
    backend = ThreadingHTTPServer(('127.0.0.1',0),App)
    threading.Thread(target=backend.serve_forever,daemon=True).start()
    config = directory/'settings.json'
    config.write_text(json.dumps({'listen_url':base,'public_url':base,'outbound_url':f'http://127.0.0.1:{backend.server_port}/prompt','api_token':app_token,'whisper_model':str(args.model.resolve()),'speakers':[{'id':'smoke-speaker','name':'Test room','tags':['verification'],'token':secrets.token_hex(32),'enabled':True}]}),encoding='utf-8')
    hidden = subprocess.CREATE_NO_WINDOW if hasattr(subprocess,'CREATE_NO_WINDOW') else 0
    started = time.monotonic()
    with (directory/'hub.log').open('w',encoding='utf-8') as log:
        process = subprocess.Popen([str(args.exe.resolve()),'--config',str(config.resolve())],stdout=log,stderr=log,creationflags=hidden)
        try:
            deadline = time.monotonic()+15
            while True:
                if process.poll() is not None: raise RuntimeError('Hub failed to start; inspect the smoke log.')
                try:
                    with urllib.request.urlopen(base+'/healthz',timeout=1): break
                except OSError:
                    if time.monotonic()>deadline: raise RuntimeError('Hub startup timed out')
                    time.sleep(.1)
            input_wav = directory/'input.wav'; output_wav = directory/'reply.wav'
            subprocess.run([str(args.exe.resolve()),'--tts','Please tell me the weather, Speaker.','--output',str(input_wav)],check=True,timeout=30,creationflags=hidden)
            spec = importlib.util.spec_from_file_location('simulator',Path(__file__).with_name('simulate-speaker.py'))
            simulator = importlib.util.module_from_spec(spec); spec.loader.exec_module(simulator)
            asyncio.run(simulator.simulate(SimpleNamespace(config=config,hub=base,speaker='smoke-speaker',wav=input_wav,output=output_wav,timeout=90)))
            if failures: raise RuntimeError('; '.join(failures))
            if len(received)!=1: raise RuntimeError(f'Expected one app prompt; got {len(received)}')
            prompt = received[0]
            if prompt['speaker']!={'id':'smoke-speaker','name':'Test room','tags':['verification']}: raise RuntimeError('Speaker identity mismatch')
            if prompt['wake_name'].lower() not in ('speaker','computer'): raise RuntimeError('Wake name was not detected')
            with wave.open(str(output_wav),'rb') as audio:
                if (audio.getframerate(),audio.getnchannels(),audio.getsampwidth())!=(16000,1,2): raise RuntimeError('Incorrect reply format')
                report['reply_seconds'] = round(audio.getnframes()/16000,3)
            report.update(success=True,recognized_text=prompt['text'],speaker=prompt['speaker'],wake_name=prompt['wake_name'],reply_file=str(output_wav.resolve()),elapsed_seconds=round(time.monotonic()-started,3))
        finally:
            process.terminate(); process.wait(timeout=10); backend.shutdown(); backend.server_close()
    # Only the CLI explicitly prints synthetic test transcripts. The background hub must not.
    hub_log = (directory/'hub.log').read_text(encoding='utf-8')
    if 'weather' in hub_log.lower() or prompt['text'].lower() in hub_log.lower(): raise RuntimeError('Native speech logs leaked a transcript')
    (directory/'report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(json.dumps(report,indent=2))
    print('Report:',directory/'report.json')
if __name__=='__main__': main()
