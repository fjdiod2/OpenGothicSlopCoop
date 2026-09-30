#!/usr/bin/env python3
"""Start one authenticated co-op Host or Join window from a local engine build."""
import argparse
import configparser
import contextlib
import datetime
import getpass
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import signal
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent


def default_state():
    if sys.platform == 'win32':
        return Path(os.environ.get('LOCALAPPDATA', Path.home() / 'AppData/Local')) / 'OpenGothic Co-op'
    if sys.platform == 'darwin':
        return Path.home() / 'Library/Application Support/OpenGothic Co-op'
    return Path(os.environ.get('XDG_STATE_HOME', Path.home() / '.local/state')) / 'opengothic-coop'


def executable(path=None, name='Gothic2Notr'):
    if path:
        candidate = Path(path).expanduser().resolve()
        if candidate.is_file():
            return candidate
        raise ValueError(f'Executable does not exist: {candidate}')
    suffix = '.exe' if sys.platform == 'win32' else ''
    for directory in ('build/opengothic', 'build/opengothic/RelWithDebInfo',
                      'build/opengothic/Release', 'build-stock/opengothic'):
        candidate = ROOT / directory / (name + suffix)
        if candidate.is_file():
            return candidate.resolve()
    raise ValueError(f'Build {name} first, or provide its executable path')


def child(parent, name):
    return next((p for p in parent.iterdir() if p.name.lower() == name.lower()), parent / name)


def game_data(path):
    path = Path(path).expanduser().resolve()
    if not path.is_dir() or not child(path, 'Data').is_dir() or not child(path, '_work').is_dir():
        raise ValueError('Game-data folder must contain Data and _work from your Gothic II installation')
    return path


def settings(runtime):
    path = runtime / 'Gothic.ini'
    ini = configparser.ConfigParser(interpolation=None)
    ini.read(path, encoding='utf-8')
    for section, values in {
        'ENGINE': {'zMaxFps': '30'},
        'INTERNAL': {'vidResIndex': '2'},
        'SOUND': {'soundEnabled': '1', 'soundVolume': '0.7', 'musicEnabled': '1', 'musicVolume': '0.5'},
    }.items():
        section = next((s for s in ini.sections() if s.lower() == section.lower()), section)
        if not ini.has_section(section):
            ini.add_section(section)
        for key, value in values.items():
            if not ini.has_option(section, key):
                ini.set(section, key, value)
    with path.open('w', encoding='utf-8') as output:
        ini.write(output)


@contextlib.contextmanager
def runtime_lock(runtime):
    with (runtime / '.launcher.lock').open('a+b') as lock:
        try:
            if sys.platform == 'win32':
                import msvcrt
                lock.seek(0)
                if not lock.read(1):
                    lock.write(b'0'); lock.flush()
                lock.seek(0)
                msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            raise ValueError('This co-op profile is already running; choose another --profile') from error
        yield


def read_secret(path):
    value = path.read_text(encoding='ascii').strip()
    if not re.fullmatch(r'[0-9a-fA-F]{64}', value):
        raise ValueError('Invitation must be exactly 64 hexadecimal digits; ask the host for a fresh invitation')
    return value.lower()


def write_private(path, value):
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(descriptor, 'w', encoding='ascii') as output:
        output.write(value + '\n')


def prepare(args):
    runtime = args.state_dir.expanduser().resolve() / args.profile / args.role
    runtime.mkdir(parents=True, exist_ok=True, mode=0o700)
    config_path = runtime / 'launcher.json'
    config = json.loads(config_path.read_text()) if config_path.exists() else {}
    supplied_data = args.data or config.get('data')
    if not supplied_data:
        supplied_data = input('Gothic II game-data folder: ').strip().strip('"')
    data = game_data(supplied_data)
    if runtime == data or data in runtime.parents:
        raise ValueError('Co-op settings/saves must be outside the game installation')
    binary = executable(args.engine)
    settings(runtime)
    invitation = runtime / 'invitation.txt'
    if args.secret_file:
        secret = read_secret(args.secret_file.expanduser().resolve())
    elif invitation.exists():
        secret = read_secret(invitation)
    elif args.role == 'host':
        secret = secrets.token_hex(32)
        write_private(invitation, secret)
    else:
        secret = getpass.getpass('Invitation secret from host (hidden): ').strip().lower()
        if not re.fullmatch(r'[0-9a-f]{64}', secret):
            raise ValueError('Invitation must be 64 hexadecimal digits')
        write_private(invitation, secret)
    command = [str(binary), '-g', str(data), '-window']
    if args.role == 'host':
        command += ['-coop-host', f'{args.listen}:{args.port}']
        checkpoint = args.load
        if not checkpoint and not args.new and sys.stdin.isatty():
            selected = input('Checkpoint path (Enter for new game): ').strip().strip('"')
            if selected:
                checkpoint = Path(selected)
        if checkpoint:
            checkpoint = checkpoint.expanduser().resolve()
            if not checkpoint.is_file():
                raise ValueError(f'Checkpoint does not exist: {checkpoint}')
            # Import to a fresh slot; never replace an existing profile save or source.
            slot = next(i for i in range(100, 10000) if not (runtime / f'save_slot_{i}.sav').exists())
            shutil.copy2(checkpoint, runtime / f'save_slot_{slot}.sav')
            command += ['-save', str(slot)]
        else:
            command += ['-nomenu']
            if args.world:
                command += ['-w', args.world]
    else:
        address = args.address or config.get('address') or input('Host public IPv4 or hostname: ').strip()
        if ':' in address:
            host, port = address.rsplit(':', 1)
            if not port.isdigit() or not 1 <= int(port) <= 65535:
                raise ValueError('Host port must be in 1..65535')
        else:
            host, port = address, str(args.port)
        if not re.fullmatch(r'[A-Za-z0-9.-]{1,253}', host):
            raise ValueError('Enter an IPv4 address or hostname, with optional :port')
        endpoint = f'{host}:{port}'
        config['address'] = endpoint
        command += ['-coop-join', endpoint]
    config['data'] = str(data)
    config_path.write_text(json.dumps(config, indent=2) + '\n')
    environment = os.environ.copy()
    environment['OPENGOTHIC_JOIN_SECRET'] = secret
    if args.runtime_bin:
        environment['PATH'] = str(args.runtime_bin.expanduser().resolve()) + os.pathsep + environment.get('PATH', '')
    return runtime, command, environment, invitation


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('role', choices=('host', 'join'))
    parser.add_argument('--data', type=Path, help='Your own Gothic II installation or prepared arena-data copy')
    parser.add_argument('--engine', type=Path, help='Locally built Gothic2Notr executable')
    parser.add_argument('--state-dir', type=Path, default=default_state())
    parser.add_argument('--profile', default='default', help='Separate campaign/settings profile')
    parser.add_argument('--port', type=int, default=27500)
    parser.add_argument('--listen', default='0.0.0.0', help='Host local IPv4 interface (default: all)')
    parser.add_argument('--address', help='Join: host public IPv4 or hostname, optionally :port')
    parser.add_argument('--secret-file', type=Path, help='Read invitation without displaying it or using command arguments')
    parser.add_argument('--runtime-bin', type=Path, help='Windows compiler/OpenSSL DLL folder, e.g. C:/msys64/mingw64/bin')
    start = parser.add_mutually_exclusive_group()
    start.add_argument('--new', action='store_true', help='Host: start a new game in this profile')
    start.add_argument('--load', type=Path, help='Host: copy and load a checkpoint, preserving the source')
    parser.add_argument('--world', help='Host new game world (arena: coop_test.zen)')
    args = parser.parse_args(argv)
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,64}', args.profile) or not 1 <= args.port <= 65535:
        parser.error('Use a simple profile name and a port in 1..65535')
    if args.role == 'join' and (args.new or args.load or args.world):
        parser.error('--new, --load and --world belong to Host')
    runtime = args.state_dir.expanduser().resolve() / args.profile / args.role
    if args.data and (args.data.expanduser().resolve() == runtime or args.data.expanduser().resolve() in runtime.parents):
        parser.error("Co-op profile must be outside the installation")
    runtime.mkdir(parents=True, exist_ok=True, mode=0o700)
    process = None
    guard = None
    def interrupted(_signal, _frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, interrupted)
    try:
        guard = runtime_lock(runtime)
        guard.__enter__()
        runtime, command, environment, invitation = prepare(args)
        stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
        log_path = runtime / f'launch-{stamp}.log'
        print(f'Settings, saves and logs: {runtime}', flush=True)
        if args.role == 'host':
            print(f'Listening on {args.listen}:{args.port}. Share your public address and port privately.', flush=True)
            print(f'Invitation: copy the contents of {args.secret_file or invitation} privately to your friend.', flush=True)
        print('Save using the host game menu. Guest: press R after a disconnect or host load.\n'
              'Close the game to finish; Ctrl-C closes this window without autosaving.', flush=True)
        with log_path.open('w', encoding='utf-8') as log:
            process = subprocess.Popen(command, cwd=runtime, env=environment, stdout=subprocess.PIPE,
                                       stderr=subprocess.STDOUT, text=True, errors='replace')
            for line in process.stdout:
                log.write(line); log.flush()
                if 'Coop:' in line or 'crashlog' in line or 'error' in line.lower():
                    print(line.rstrip(), flush=True)
            code = process.wait()
        print(f'Game exited ({code}). Log: {log_path}', flush=True)
        return code
    except (OSError, ValueError, EOFError) as error:
        print(f'Cannot launch co-op: {error}', file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print('Closing the owned game window; no automatic save.', flush=True)
        return 130
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait(timeout=5)
        if guard is not None:
            guard.__exit__(None, None, None)


if __name__ == '__main__':
    raise SystemExit(main())
