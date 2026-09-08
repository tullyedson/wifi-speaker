"""A protocol-compatible test speaker. Streams a WAV and saves one spoken reply."""
import argparse, asyncio, json, time, urllib.request, wave
from pathlib import Path
from urllib.parse import urlsplit
from websockets.asyncio.client import connect

async def simulate(args):
    settings = json.loads(args.config.read_text(encoding='utf-8-sig'))
    speaker = next((s for s in settings['speakers'] if s['id'] == args.speaker), None)
    if not speaker:
        raise SystemExit('Speaker ID is not present in the settings file.')
    base = args.hub.rstrip('/')
    headers = {'Authorization': 'Bearer ' + speaker['token']}
    with wave.open(str(args.wav), 'rb') as source:
        if (source.getframerate(), source.getnchannels(), source.getsampwidth()) != (16000,1,2):
            raise SystemExit('Input must be a 16000 Hz, mono, signed 16-bit WAV.')
        pcm = source.readframes(source.getnframes())
    uri = base.replace('http://','ws://',1) + '/v1/speakers/' + args.speaker + '/audio'
    async with connect(uri, additional_headers=headers, max_size=6400) as socket:
        await socket.send(json.dumps({'type':'hello','protocol':1,'speaker_id':args.speaker,'firmware':'python-simulator/0.1.0','sample_rate':16000,'channels':1,'sample_format':'s16le'}))
        while json.loads(await socket.recv())['type'] != 'ready':
            pass
        async def upload():
            payload = bytes(16000) + pcm + bytes(48000)
            start = time.monotonic()
            for index in range(0,len(payload),640):
                await socket.send(payload[index:index+640])
                await asyncio.sleep(max(0, start + (index+640)/32000 - time.monotonic()))
        sender = asyncio.create_task(upload())
        waiting_for_backend = False
        try:
            async with asyncio.timeout(args.timeout):
                async for raw in socket:
                    event = json.loads(raw)
                    if event['type'] == 'state':
                        print('State:', event['state'], flush=True)
                        if event['state'] == 'waiting':
                            waiting_for_backend = True
                        elif event['state'] == 'listening' and waiting_for_backend:
                            def fetch_error():
                                request = urllib.request.Request(base + '/v1/speakers', headers={'Authorization': 'Bearer ' + settings['api_token']})
                                with urllib.request.urlopen(request, timeout=5) as response:
                                    status = json.load(response)
                                return next(s['last_error'] for s in status['speakers'] if s['speaker']['id'] == args.speaker)
                            error = await asyncio.to_thread(fetch_error)
                            if error:
                                raise RuntimeError(error)
                    if event['type'] == 'play':
                        if urlsplit(event['audio_url']).netloc != urlsplit(base).netloc:
                            raise RuntimeError('Audio URL does not match the configured hub.')
                        def fetch():
                            request = urllib.request.Request(event['audio_url'],headers=headers)
                            with urllib.request.urlopen(request,timeout=10) as response:
                                audio = response.read(6_000_001)
                            if len(audio) > 6_000_000: raise RuntimeError('Response audio is too large.')
                            return audio
                        await socket.send(json.dumps({'type':'playback_started','request_id':event['request_id']}))
                        audio = await asyncio.to_thread(fetch)
                        if audio[:4] != b'RIFF': raise RuntimeError('Response is not WAV audio.')
                        args.output.parent.mkdir(parents=True,exist_ok=True); args.output.write_bytes(audio)
                        await socket.send(json.dumps({'type':'playback_finished','request_id':event['request_id']}))
                        print('Reply saved to', args.output, flush=True)
                        return
        finally:
            sender.cancel()
            try: await sender
            except asyncio.CancelledError: pass
    raise RuntimeError('Connection closed before a reply arrived.')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', required=True, type=Path, help='Private hub settings containing the registered speaker token')
    parser.add_argument('--hub', default='http://127.0.0.1:48490')
    parser.add_argument('--speaker', required=True)
    parser.add_argument('--wav', required=True,type=Path)
    parser.add_argument('--output',default=Path('runtime/simulator-reply.wav'),type=Path)
    parser.add_argument('--timeout',default=120,type=int)
    asyncio.run(simulate(parser.parse_args()))
if __name__ == '__main__':
    main()
