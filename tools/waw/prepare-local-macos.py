from pathlib import Path
import datetime
import hashlib
import json
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'build/macos-xcode/bin/RelWithDebInfo/Vita3K.app'
STAMP = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
OUTPUT = ROOT / 'build/waw-local-bundles' / STAMP / 'Vita3K.app'
if OUTPUT.exists():
    raise SystemExit('Existing staged bundle is preserved')

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def dependencies(path):
    text = subprocess.check_output(['otool', '-L', str(path)], text=True)
    rows = []
    for line in text.splitlines()[1:]:
        match = re.fullmatch(r'\s*(.*?) \(compatibility version [^,]+, current version ([0-9.]+)(?:, weak)?\)', line)
        if not match:
            raise RuntimeError('Unrecognized dependency: ' + line)
        rows.append((match[1], match[2]))
    return rows

main = SOURCE / 'Contents/MacOS/Vita3K'
changes = []
for name, version in dependencies(main):
    match = re.search(r'(Qt\w+\.framework/Versions/[^/]+/Qt\w+)$', name)
    if not match:
        continue
    provider = Path('/opt/homebrew/lib') / match[1]
    if not provider.is_file():
        raise RuntimeError('Missing installed provider: ' + str(provider))
    canonical, actual_version = dependencies(provider)[0]
    if actual_version != version or not canonical.startswith('/opt/homebrew/'):
        raise RuntimeError(('Qt provider mismatch', name, version, canonical, actual_version))
    changes.append({'old': name, 'new': canonical, 'version': version,
                    'provider_sha256': digest(provider)})
if len(changes) != 7:
    raise RuntimeError(('Unexpected direct Qt dependency count', len(changes)))

(OUTPUT / 'Contents/MacOS').mkdir(parents=True)
shutil.copy2(SOURCE / 'Contents/Info.plist', OUTPUT / 'Contents/Info.plist')
shutil.copytree(SOURCE / 'Contents/Resources', OUTPUT / 'Contents/Resources', symlinks=True)
shutil.copy2(main, OUTPUT / 'Contents/MacOS/Vita3K')
(OUTPUT / 'Contents/Frameworks').mkdir()
shutil.copy2(SOURCE / 'Contents/Frameworks/libMoltenVK.dylib', OUTPUT / 'Contents/Frameworks/libMoltenVK.dylib')
(OUTPUT / 'Contents/Resources/qt.conf').write_text('[Paths]\nPlugins = /opt/homebrew/share/qt/plugins\n')
command = ['install_name_tool']
for change in changes:
    command.extend(['-change', change['old'], change['new']])
command.append(str(OUTPUT / 'Contents/MacOS/Vita3K'))
subprocess.run(command, check=True)
actual = dict(dependencies(OUTPUT / 'Contents/MacOS/Vita3K'))
assert all(actual.get(change['new']) == change['version'] for change in changes)
report = {'source': str(SOURCE), 'staged_app': str(OUTPUT), 'source_binary_sha256': digest(main),
          'staged_binary_sha256': digest(OUTPUT / 'Contents/MacOS/Vita3K'),
          'qt_binding': changes, 'requires_installed_homebrew_qt': True,
          'signed': False, 'launched': False}
(OUTPUT.parent / 'provenance.json').write_text(json.dumps(report, indent=2) + '\n')
(ROOT / 'build/waw-checks/local-bundle.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2), flush=True)
