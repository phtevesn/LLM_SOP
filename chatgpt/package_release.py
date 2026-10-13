#!/usr/bin/env python3
"""Build a deterministic source-only ZIP; hash every packaged payload file."""
import argparse
import hashlib
import json
from pathlib import Path
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parent
    output = (args.output or root.parent / 'sop_solver.zip').resolve()
    top = ['sop_solver.cpp', 'README.md', 'Makefile', 'test_solver.py',
           'test_bounds.cpp', 'test_engines.cpp', 'package_release.py']
    files = [root / name for name in top]
    allowed = {'.cpp', '.py', '.md', '.txt', '.csv', '.log', '.json', '.sop'}
    for folder in ('examples', 'benchmarks'):
        files.extend(p for p in (root / folder).rglob('*')
                     if p.is_file() and not p.is_symlink()
                     and '__pycache__' not in p.parts and p.suffix in allowed)
    if output in files or (output.exists() and any(output.samefile(p) for p in files)):
        parser.error('Archive must not overwrite a payload file')
    payload = {p.relative_to(root).as_posix(): p.read_bytes() for p in sorted(files)}
    manifest = {'format': 1, 'sha256': {
        name: hashlib.sha256(data).hexdigest() for name, data in payload.items()}}
    payload['MANIFEST.json'] = (json.dumps(manifest, indent=2, sort_keys=True) + '\n').encode()
    with zipfile.ZipFile(output, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(payload.items()):
            info = zipfile.ZipInfo('sop_solver/' + name, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            archive.writestr(info, data)
    # Verify bytes, not merely the archive directory or CRC.
    with zipfile.ZipFile(output) as archive:
        for name, expected in manifest['sha256'].items():
            actual = hashlib.sha256(archive.read('sop_solver/' + name)).hexdigest()
            if actual != expected:
                raise RuntimeError('Archive hash mismatch: ' + name)
    print(f'{output}: {len(manifest["sha256"])} verified payload files')


if __name__ == '__main__':
    main()
