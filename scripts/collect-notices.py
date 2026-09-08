"""Collect license notices from locally available locked dependency sources."""
import os, shutil, tomllib
from pathlib import Path

root = Path(__file__).resolve().parents[1]
destination = root/'docs'/'third-party'
registry = Path(os.environ.get('CARGO_HOME',str(Path.home()/'.cargo')))/'registry'/'src'
index = ['# Locally collected dependency notices', '', 'Exact versions come from Cargo.lock. These are the upstream license/notice files available in the build sources. Packages not used on Windows may not be downloaded.', '']
count = 0
def collect(label, folder):
    global count
    files = [p for p in folder.iterdir() if p.is_file() and p.name.lower().startswith(('license','licence','copying','notice','copyright'))]
    if not files: return []
    target = destination/label; target.mkdir(parents=True,exist_ok=True)
    for source in files:
        shutil.copyfile(source,target/source.name); count += 1
    return [p.name for p in files]
for package in tomllib.loads((root/'Cargo.lock').read_text(encoding='utf-8'))['package']:
    if not package.get('source','').startswith('registry+'): continue
    label = package['name']+'-'+package['version']
    folder = next((parent/label for parent in registry.iterdir() if (parent/label).is_dir()),None)
    if folder:
        licenses = collect(label,folder)
        manifest = tomllib.loads((folder/'Cargo.toml').read_text(encoding='utf-8'))
        license_name = manifest['package'].get('license','See upstream package')
        index.append(f"- {label}: {license_name}. " + (', '.join(f'[{name}]({label}/{name})' for name in licenses) if licenses else 'Consult upstream source.'))
        if package['name']=='whisper-rs-sys': collect('whisper.cpp',folder/'whisper.cpp')
for folder in (root/'firmware'/'.pio'/'libdeps'/'muse_luxe').iterdir():
    if folder.is_dir():
        files = collect('firmware-'+folder.name.replace(' ','-'),folder)
        if files: index.append('- Firmware '+folder.name+': '+', '.join(files))
framework = root/'.tools'/'platformio'/'packages'/'framework-arduinoespressif32'
if framework.is_dir(): collect('Arduino-ESP32',framework)
destination.mkdir(parents=True,exist_ok=True)
(destination/'README.md').write_text('\n'.join(index)+'\n',encoding='utf-8')
print(f'Collected {count} upstream notices under docs/third-party')
