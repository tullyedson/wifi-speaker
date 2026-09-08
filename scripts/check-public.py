"""Check tracked files for accidentally committed local configuration and secrets."""
import ipaddress
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
names = subprocess.check_output(['git', 'ls-files', '-z'], cwd=root).decode('utf-8').split('\0')
problems = []
checked = 0
private_dirs = {'runtime', 'backups', 'artifacts', 'models', 'target', '.tools', '.pio', 'node_modules'}
private_suffixes = {'.bin', '.wav', '.pcm', '.log', '.bundle', '.sqlite', '.sqlite3', '.db', '.pem', '.key', '.pfx', '.p12'}
local_address = re.compile(r'(?<![\w.])(?:\d{1,3}\.){3}\d{1,3}(?![\w.])')
workstation_path = re.compile(r'(?i)(?<![\w/])[a-z]:[\\/]|/(?:Users|home)/[^\s/]+/')
secret_patterns = [
    re.compile(r'-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----'),
    re.compile(r'\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{50,}|sk-[A-Za-z0-9_-]{24,})\b'),
    re.compile(r'https?://[^\s/"<>]+:[^\s/"<>]+@'),
]
allowed_addresses = {'0.0.0.0', '127.0.0.1', '192.168.4.1'}
private_networks = tuple(ipaddress.ip_network(value) for value in ('10.0.0.0/8', '172.16.0.0/12', '192.168.0.0/16', '169.254.0.0/16', '100.64.0.0/10'))
for name in filter(None, names):
    path = root / name
    lower = path.name.lower()
    if private_dirs.intersection(Path(name).parts) or path.suffix.lower() in private_suffixes or lower == 'settings.json' or lower.startswith('settings-') or (lower.startswith('.env') and lower != '.env.example') or ('provision' in lower and lower.endswith('.json')):
        problems.append((name, 'private runtime file is tracked'))
    if not path.is_file():
        problems.append((name, 'tracked file is missing'))
        continue
    content = path.read_bytes()
    checked += 1
    for pattern in secret_patterns:
        if pattern.search(content.decode('utf-8', errors='replace')):
            problems.append((name, 'credential-like content'))
    if name.startswith('docs/third-party/'):
        continue  # Upstream attribution and notice text must remain intact.
    if b'\0' in content:
        if name not in {'desktop/icons/icon.ico', 'desktop/icons/icon.png'}:
            problems.append((name, 'unexpected binary source file'))
        continue
    text = content.decode('utf-8')
    if workstation_path.search(text):
        problems.append((name, 'absolute workstation path'))
    for match in local_address.finditer(text):
        try:
            address = ipaddress.ip_address(match.group())
        except ValueError:
            continue  # Dependency version strings can resemble addresses.
        if str(address) not in allowed_addresses and any(address in network for network in private_networks):
            # Network definitions in this scanner are patterns, not destinations.
            if name != 'scripts/check-public.py':
                problems.append((name, 'local network address'))
                break

for name, reason in sorted(set(problems)):
    print(f'{name}: {reason}')  # Never echo the potentially sensitive value.
print(f'Checked {checked} tracked files; {len(set(problems))} finding(s).')
sys.exit(bool(problems))
