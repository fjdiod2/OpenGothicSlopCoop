#!/usr/bin/env python3
"""Make an isolated game-data copy with the deterministic optional co-op arena."""
import argparse
import hashlib
from pathlib import Path
import runpy
import shutil
import subprocess
import sys

api = runpy.run_path(str(Path(__file__).with_name('coop-launch.py')))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True, help='New folder outside the original installation')
    parser.add_argument('--generator', type=Path, help='Locally built create-coop-arena executable')
    args = parser.parse_args()
    source = api['game_data'](args.data)
    output = args.output.expanduser().resolve()
    if output.exists() or source == output or source in output.parents:
        parser.error('Output must be a new folder outside the installation')
    generator = api['executable'](args.generator, 'create-coop-arena')
    output.mkdir(parents=True)
    for name in ('Data', '_work', 'system'):
        entry = api['child'](source, name)
        if not entry.is_dir():
            parser.error(f'Missing installation folder: {name}')
        if sys.platform == "darwin":
            subprocess.run(["/bin/cp", "-cR", str(entry), str(output / entry.name)], check=True)
        else:
            shutil.copytree(entry, output / entry.name)
    data = api['child'](api['child'](output, '_work'), 'Data')
    worlds = api['child'](data, 'Worlds')
    worlds.mkdir(exist_ok=True)
    arena = worlds / 'COOP_TEST.ZEN'
    subprocess.run([str(generator), str(arena)], check=True)
    print(f'Arena SHA-256: {hashlib.sha256(arena.read_bytes()).hexdigest()}')
    print(f'Use --data "{output}" and Host --new --world coop_test.zen')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
