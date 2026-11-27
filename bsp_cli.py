#!/usr/bin/env python3
"""lean-bsp entry point of the linux-host BSP: capabilities, build, load and run.

Every verb ends its output with one JSON line, the result lean-bsp parses."""

import argparse
import hashlib
import json
import os
import shlex
import signal
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent
TARGET = 'native'
WRAPPED = ('malloc', 'calloc', 'realloc', 'free')  # heap use, counted by lean-bsp's src/bsp_heap.c
HEAP_BLOCKS = 4096  # live blocks the count can follow: a host application may hold more than a board's
STATE = 'linux-host.json'  # this BSP's own record of what it built, in the build directory


def result(status: str, **fields) -> int:
    print(json.dumps({'status': status, **fields}), flush=True)
    return 1 if status == 'error' else 0


def echo_run(cmd: list[str]) -> bool:
    print(shlex.join(cmd), flush=True)
    return subprocess.run(cmd, check=False).returncode == 0


def cmd_capabilities(args) -> int:
    return result('ok')


def cmd_build(args) -> int:
    if args.target != TARGET:
        return result('error', message=f'linux-host has a single target, {TARGET!r}')
    app = json.loads(Path(args.app_json).read_text(encoding='utf-8'))
    if app.get('linker_script'):
        return result('error', message='linux-host links with the host defaults and takes no custom linker script')
    bdir = Path(args.bdir)
    obj_dir = bdir / 'obj'
    obj_dir.mkdir(parents=True, exist_ok=True)
    cc = os.environ.get('CC', 'gcc')
    cflags = shlex.split(os.environ.get('CFLAGS', '-Og -g'))
    warnings = ['-Wall', '-Wextra']
    bsp_flags = [*cflags, *warnings, f'-I{ROOT / "include"}', f'-DLEAN_BSP_HEAP_MAX_BLOCKS={HEAP_BLOCKS}']
    app_flags = [*bsp_flags, *(f'-I{d}' for d in app['include_dirs']), *(f'-D{d}' for d in app['defines'])]
    units = [(src, bsp_flags) for src in sorted((ROOT / 'src').glob('*.c'))]
    units += [(Path(src), app_flags) for src in app['sources']]
    objects = []
    for src, flags in units:
        tag = hashlib.sha1(str(src).encode()).hexdigest()[:8]  # same file name in two directories
        obj = obj_dir / f'{src.stem}-{tag}.o'
        if not echo_run([cc, *flags, '-c', str(src), '-o', str(obj)]):
            return result('error', message=f'compiling {src} failed')
        objects.append(str(obj))
    exe = bdir / app['name']
    libs = [*app.get('link_flags', []), app['lib']] if app.get('lib') else []  # an archive of its own ([app.build])
    link = [cc, *cflags, *objects, *libs, '-pthread', *(f'-Wl,--wrap={f}' for f in WRAPPED), '-o', str(exe)]
    if not echo_run(link):
        return result('error', message='linking failed')
    (bdir / STATE).write_text(json.dumps({'exe': str(exe)}) + '\n', encoding='utf-8')
    return result('ok', artifact={'elf': str(exe), 'bin': None, 'hex': None})


def built_exe(bdir: Path) -> Path | None:
    state = bdir / STATE
    if not state.is_file():
        return None
    return Path(json.loads(state.read_text(encoding='utf-8'))['exe'])


def cmd_load(args) -> int:
    if built_exe(Path(args.bdir)) is None:
        return result('error', message=f'nothing built in {args.bdir}')
    return result('ok', message='nothing to load: linux-host runs the build in place')


def run_detached(exe: Path, bdir: Path) -> int:
    """UART 0 becomes a pseudo-terminal; return as soon as the application has opened it."""
    pty_file = bdir / 'uart0-pty'
    pty_file.unlink(missing_ok=True)
    log_path = bdir / 'run.log'
    env = {**os.environ, 'LEAN_BSP_UART0': 'pty', 'LEAN_BSP_UART0_PTY_FILE': str(pty_file)}
    with log_path.open('wb') as log:
        proc = subprocess.Popen(
            [str(exe)], stdin=subprocess.DEVNULL, stdout=log, stderr=log, env=env, start_new_session=True
        )
    while not pty_file.exists():
        if proc.poll() is not None:
            return result(
                'error', message=f'the application exited with code {proc.returncode} before opening UART 0, see {log_path}'
            )
        time.sleep(0.01)
    com_device = pty_file.read_text(encoding='utf-8').strip()
    return result('ok', com_device=com_device, exit_code=None, pid=proc.pid, log=str(log_path))


def run_attached(exe: Path, timeout: float | None) -> int:
    """UART 0 is this process's stdin/stdout; wait for the application to exit."""
    proc = subprocess.Popen([str(exe)])
    previous = signal.signal(signal.SIGINT, signal.SIG_IGN)  # Ctrl-C is for the application, not for us
    try:
        returncode = proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        return result('error', message=f'the application was still running after {timeout} s and was killed',
                      com_device=None, exit_code=None)
    finally:
        signal.signal(signal.SIGINT, previous)
    print(flush=True)  # the application's last line may lack a newline; keep the JSON on a line of its own
    return result('ok', com_device=None, exit_code=returncode)


def cmd_run(args) -> int:
    bdir = Path(args.bdir)
    exe = built_exe(bdir)
    if exe is None:
        return result('error', message=f'nothing built in {bdir}')
    if args.detach:
        return run_detached(exe, bdir)
    timeout = args.timeout if args.timeout is not None and args.timeout >= 0 else None
    return run_attached(exe, timeout)


def main() -> int:
    parser = argparse.ArgumentParser(prog='bsp_cli.py', description='linux-host BSP entry point for lean-bsp')
    sub = parser.add_subparsers(dest='verb', required=True)
    p = sub.add_parser('capabilities')
    p.add_argument('--json', action='store_true')
    p.set_defaults(func=cmd_capabilities)
    p = sub.add_parser('build')
    p.add_argument('--app-json', required=True)
    p.add_argument('--target', required=True)
    p.add_argument('--bdir', required=True)
    p.set_defaults(func=cmd_build)
    p = sub.add_parser('load')
    p.add_argument('--bdir', required=True)
    p.add_argument('--target', default=TARGET)
    p.set_defaults(func=cmd_load)
    p = sub.add_parser('run')
    p.add_argument('--bdir', required=True)
    p.add_argument('--target', default=TARGET)
    p.add_argument('--detach', action='store_true')
    p.add_argument('--timeout', type=float, help='seconds; -1 or absent: wait forever')
    p.add_argument('--until', help='ignored: a native process ends by itself, and its output is the terminal')
    p.set_defaults(func=cmd_run)
    args = parser.parse_args()
    return args.func(args)


if __name__ == '__main__':
    sys.exit(main())
