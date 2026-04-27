#!/usr/bin/env python3
"""
bot_test_runner.py — drive openjkded.x86.exe across every installed CTF map
and record whether the v2 bot scores a flag capture inside a per-map timeout.

How a single map run works
--------------------------
1. Launch openjkded with a unique net_port, sv_pure 0, capturelimit 1, an
   rconpassword, and bot_forcebot2 1.  Note we do NOT use bot_minplayers —
   bots are added explicitly so we know exactly when there's one bot, what
   its name is, and which team it's on.
2. Stream the server's stdout in a background thread to both a per-map log
   file (bot_test_logs/<mapname>.log) and an in-memory queue.
3. Wait for the "------ Server Initialization ------" line, then send
   `kickall` and `addbot <name> 5 r` over rcon UDP.
4. Watch the queue for "Exit: Capturelimit hit." — the engine's native
   exit-condition log (G_LogPrintf in g_main.c::LogExit) which fires
   when level.teamScores[X] >= capturelimit.  With capturelimit 1 and a
   single bot in play, this fires on the first successful cap.  Hit a
   wall-clock deadline on failure.  Kill the server, record the row,
   move on.

Parallel mode
-------------
With --parallel N > 1 the runner uses a thread pool of N workers.  Each
worker gets a unique net_port (base + worker_idx) and a unique fs_homepath
(under bot_test_logs/home_W<i>/) so concurrent servers don't fight over
qconsole.log or per-server config writes.  CPU sim is single-threaded per
server, so N should not exceed your physical core count.

CSV is flushed after every row, so Ctrl-C always leaves a usable file.
"""

from __future__ import annotations

import argparse
import csv
import os
import queue
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
import zipfile
from datetime import datetime
from pathlib import Path
from typing import Optional


# ---- regexes ----------------------------------------------------------------

# Capture detection: with capturelimit 1, the engine's CheckExitRules path in
# g_main.c calls LogExit("Capturelimit hit.") which routes through
# G_LogPrintf -> trap->Print and surfaces in the dedicated server's stdout as
# "Exit: Capturelimit hit.".  This is engine-native — no game-source
# instrumentation required, so the bot mod stays drop-in friendly.
#
# Note: the engine doesn't include the capturer's clientNum or team in this
# message.  With kickall + a single addbot, those are inferable: the only bot
# on the server is the capturer.  cap_team / cap_capturer columns are kept in
# the CSV for compatibility but populated empty.
CAP_RE          = re.compile(r'Exit:\s*Capturelimit hit', re.IGNORECASE)
SERVER_READY_RE = re.compile(r'-+ Server Initialization -+', re.IGNORECASE)
CRASH_RE        = re.compile(r'\b(Sys_Error|FATAL|ERROR: CL_ParseGamestate|Hunk_Alloc failed)\b')

# The engine prints "Opening IP socket: localhost:NNNNN" each time it tries
# to bind UDP.  When the requested net_port is taken (WSAEADDRINUSE) it
# auto-bumps and tries again.  We track the LAST successful "Opening IP
# socket" line — that's the port rcon must talk to, not necessarily the
# port we asked for on the command line.  A subsequent "WSAEADDRINUSE"
# warning invalidates the most recent attempt; the next "Opening IP socket"
# becomes the new candidate.
PORT_OPEN_RE    = re.compile(r'Opening IP socket:\s*\S+:(\d+)')
PORT_INUSE_RE   = re.compile(r'WSAEADDRINUSE')


# ---- defaults ---------------------------------------------------------------

# Repo-relative default for the dedicated server binary.  This file lives at
# <repo>/tools/bot_test_runner.py, so the build output is two levels up.
_REPO_ROOT      = Path(__file__).resolve().parent.parent
DEFAULT_BIN     = str(_REPO_ROOT / 'build' / 'Release' / 'openjkded.x86.exe')

# JKA install location is per-user and cannot be hardcoded usefully.  Allow an
# environment variable to supply it; otherwise the script will require
# --jka-base on the command line.  Set JKA_GAMEDATA to the absolute path of
# your "GameData" folder, e.g.
#   PowerShell:  $env:JKA_GAMEDATA = 'C:\Games\Jedi Academy\GameData'
#   bash:        export JKA_GAMEDATA="$HOME/.steam/steam/steamapps/common/Jedi Academy/GameData"
DEFAULT_BASE    = os.environ.get('JKA_GAMEDATA', '')

DEFAULT_PORT_BASE = 29070     # JKA's PORT_SERVER; first worker uses this

# Bot we ask the server to addbot.  Any entry in base/botfiles/bots.txt
# works; alora ships with vanilla JKA.  ('kyle' isn't a bot name — the
# Katarn one is registered as kyle_katarn.)  By default we don't pass a
# skill or team, so the engine uses its own default skill (4) and auto-
# picks the under-populated team — perfectly fine for solo CTF tests.
DEFAULT_BOT_NAME  = 'alora'
DEFAULT_BOT_SKILL: Optional[int] = None
DEFAULT_BOT_TEAM:  Optional[str] = None


# ---- map discovery ----------------------------------------------------------

def discover_ctf_maps(base_dir: Path) -> list[str]:
    """Scan every .pk3 under <base_dir>/base/ for maps/mp/ctf_*.bsp entries."""
    pk3_dir = base_dir / 'base'
    found: set[str] = set()
    for pk3 in sorted(pk3_dir.glob('*.pk3')):
        try:
            with zipfile.ZipFile(pk3) as zf:
                for name in zf.namelist():
                    n = name.lower().replace('\\', '/')
                    if n.startswith('maps/mp/ctf_') and n.endswith('.bsp'):
                        # 'maps/mp/ctf_yavin.bsp' -> 'mp/ctf_yavin'
                        found.add(n[len('maps/'):-len('.bsp')])
        except zipfile.BadZipFile:
            continue
    return sorted(found)


# ---- rcon -------------------------------------------------------------------

def rcon_send(port: int, password: str, cmd: str,
              timeout: float = 2.0) -> Optional[str]:
    """Send a single Q3-style rcon UDP command to 127.0.0.1:port.

    Quake protocol:
        send: \\xff\\xff\\xff\\xff rcon <password> <cmd>
        recv: \\xff\\xff\\xff\\xff print\\n<response>

    Returns the response text without the OOB header, or None on timeout.
    """
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(timeout)
    try:
        payload = b'\xff\xff\xff\xff' + f'rcon {password} {cmd}'.encode('utf-8')
        sock.sendto(payload, ('127.0.0.1', port))
        try:
            data, _ = sock.recvfrom(8192)
        except socket.timeout:
            return None
        # Strip the four-byte OOB header and the leading "print\n" if present.
        if data.startswith(b'\xff\xff\xff\xff'):
            data = data[4:]
        if data.startswith(b'print\n'):
            data = data[len('print\n'):]
        return data.decode('utf-8', errors='replace')
    finally:
        sock.close()


def rcon_with_retry(port: int, password: str, cmd: str,
                    attempts: int = 3, timeout: float = 1.5) -> Optional[str]:
    """rcon_send with N attempts — UDP can drop, especially on a busy machine."""
    last = None
    for _ in range(attempts):
        last = rcon_send(port, password, cmd, timeout=timeout)
        if last is not None:
            return last
    return last


# ---- server lifecycle -------------------------------------------------------

def stdout_reader(proc: subprocess.Popen,
                  line_q: 'queue.Queue[Optional[str]]',
                  log_lines: list[str],
                  log_fp) -> None:
    """Tee every stdout line into queue + tail buffer + log file."""
    try:
        for raw in iter(proc.stdout.readline, b''):
            try:
                line = raw.decode('utf-8', errors='replace').rstrip('\r\n')
            except Exception:
                line = repr(raw)
            log_lines.append(line)
            try:
                log_fp.write(line + '\n')
                log_fp.flush()
            except Exception:
                pass
            line_q.put(line)
    finally:
        line_q.put(None)


def kill_server(proc: subprocess.Popen) -> None:
    """Terminate gracefully; hard-kill if it doesn't respect terminate."""
    if proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(timeout=3.0)
    except subprocess.TimeoutExpired:
        proc.kill()
        try:
            proc.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            pass


def build_cmd(args: argparse.Namespace, mapname: str,
              port: int, rcon_password: str, homepath: Path) -> list[str]:
    """Build the openjkded command line for one map run.

    Cvars chosen so the test is reproducible AND the round can actually end:
      - capturelimit 1: round ends on first cap, freeing us to move on.
      - g_doWarmup 0:  no 20-second pre-round freeze.
      - sv_pure 0:     don't enforce pak signing on a hand-built DLL.
      - net_port:      per-worker so parallel runs don't collide.
      - fs_homepath:   per-worker so qconsole.log writes don't collide.
      - rconpassword:  per-process random secret; we use it for kickall/addbot.
    """
    return [
        args.bin,
        '+set', 'dedicated',     '2',
        '+set', 'fs_basepath',   args.jka_base,
        '+set', 'fs_homepath',   str(homepath),
        '+set', 'sv_pure',       '0',
        '+set', 'sv_maxclients', '4',
        '+set', 'net_port',      str(port),
        '+set', 'rconpassword',  rcon_password,
        '+set', 'g_gametype',    str(args.gametype),
        '+set', 'bot_enable',    '1',
        '+set', 'bot_minplayers','0',           # explicit bot management only
        '+set', 'bot_forcebot2', '1',           # force every bot to v2 AI
        '+set', 'bot_telemetry', str(args.telemetry),
        '+set', 'capturelimit',  '1',
        '+set', 'timelimit',     '0',
        '+set', 'g_doWarmup',    '0',
        '+set', 'fraglimit',     '0',
        '+map', mapname,
    ]


def run_one_map(args: argparse.Namespace, mapname: str, worker_idx: int) -> dict:
    """Run a single map and return a result row.

    worker_idx selects the net_port offset and homepath subdir.
    """
    requested_port = args.port_base + worker_idx
    # Tracks the actual UDP port the engine successfully bound to, which can
    # differ from the requested one when WSAEADDRINUSE forces a fallback.
    # Updated by the stdout reader as the engine prints "Opening IP socket".
    actual_port = requested_port
    rcon_password = f'bot_test_{os.getpid()}_{worker_idx}'

    # IMPORTANT: paths handed to the engine MUST be absolute.  The dedicated
    # server's cwd is the JKA GameData folder (so it can find base/...), so
    # any relative path we pass for fs_homepath would be resolved INSIDE the
    # JKA install — polluting the user's game directory and putting the log
    # files somewhere we don't expect.  resolve() turns them into absolute.
    log_dir   = Path(args.log_dir).resolve()
    homepath  = log_dir / f'home_W{worker_idx}'
    homepath.mkdir(parents=True, exist_ok=True)

    log_path  = log_dir / f'{mapname.replace("/", "_")}.log'
    log_path.parent.mkdir(parents=True, exist_ok=True)

    tag = f'[W{worker_idx}]'
    print(f'{tag} === {mapname} ===  (req port {requested_port}, '
          f'timeout {args.timeout}s)')

    log_lines: list[str] = []
    line_q: 'queue.Queue[Optional[str]]' = queue.Queue()

    cmd = build_cmd(args, mapname, requested_port, rcon_password, homepath)
    if args.verbose_cmd:
        print(f'{tag} cmd: {" ".join(cmd)}')

    log_fp = log_path.open('w', encoding='utf-8', errors='replace')
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        stdin=subprocess.DEVNULL,    # engine on Win uses ReadConsoleInput; pipe stdin is useless
        cwd=args.jka_base,
        bufsize=0,
        # Don't open a new console window for each child in parallel mode.
        creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0),
    )
    reader = threading.Thread(
        target=stdout_reader,
        args=(proc, line_q, log_lines, log_fp),
        daemon=True,
    )
    reader.start()

    started_at: float = time.monotonic()
    server_ready_at: Optional[float] = None
    bot_added = False
    next_probe_at: float = 0.0
    cap_at: Optional[float] = None
    cap_info: Optional[dict] = None
    timed_out = False
    crashed_line: Optional[str] = None

    # Hard wall-clock cap so a hung server can never wedge a worker.
    deadline = started_at + args.timeout + args.warmup_grace

    try:
        while True:
            if proc.poll() is not None:
                break
            if time.monotonic() >= deadline:
                timed_out = True
                break

            # Poll the queue with a small timeout so we can also drive the
            # post-ready rcon handshake without blocking on stdout.
            try:
                line = line_q.get(timeout=0.25)
            except queue.Empty:
                line = ''

            if line is None:
                break

            if line:
                if not args.quiet:
                    print(f'{tag}   | {line}')
                if server_ready_at is None and SERVER_READY_RE.search(line):
                    server_ready_at = time.monotonic()
                    # First probe attempt is `bot_add_delay` seconds out.
                    next_probe_at = server_ready_at + args.bot_add_delay
                if CRASH_RE.search(line) and crashed_line is None:
                    crashed_line = line

                # Track the engine's actual UDP bind: each "Opening IP socket"
                # is a candidate port; a follow-up WSAEADDRINUSE invalidates
                # it (the engine will print another "Opening IP socket" with
                # the bumped port).  By the time the server is responsive, the
                # MOST RECENT "Opening IP socket" not followed by an INUSE
                # warning is the port rcon must hit.
                pm = PORT_OPEN_RE.search(line)
                if pm:
                    new_port = int(pm.group(1))
                    if new_port != actual_port:
                        actual_port = new_port
                        if not args.quiet:
                            print(f'{tag}   * tracked actual bind port: '
                                  f'{actual_port}')

                if CAP_RE.search(line):
                    cap_at = time.monotonic()
                    # The engine's "Exit: Capturelimit hit." line carries no
                    # team/capturer/leveltime fields.  cap_info stays None;
                    # the CSV cap_team and cap_capturer columns will be empty.
                    break

            # ---- rcon handshake ----
            # "Server Initialization" prints at the START of map load, not the
            # end — there's no clean "load complete" log line in OpenJK.  So
            # after the initial bot_add_delay, we PROBE with `status` until we
            # get a reply, which proves the server has finished loading and is
            # processing OOB packets.  Only then do we send kickall + addbot.
            #
            # Use actual_port (parsed from "Opening IP socket" output) instead
            # of the requested port: if 29070 was taken and the engine bumped
            # to 29071, sending rcon to 29070 would silently miss.
            now = time.monotonic()
            if (server_ready_at is not None and not bot_added
                and now >= next_probe_at):
                resp = rcon_send(actual_port, rcon_password, 'status', timeout=0.5)
                if resp is None:
                    # Not responsive yet.  Re-probe in 1 s.  Don't burn rcon
                    # attempts while map media is still loading.
                    next_probe_at = now + 1.0
                else:
                    # Server is alive.  Send the actual setup commands.
                    rcon_with_retry(actual_port, rcon_password, 'kickall')

                    # `addbot <name> [skill 1-5] [team] [delay] [altname]` —
                    # see Svcmd_AddBot_f in g_bot.c.  We only append skill /
                    # team when the user explicitly set them.  With neither,
                    # the engine uses skill 4 and auto-picks the team.
                    parts = ['addbot', args.bot_name]
                    if args.bot_skill is not None:
                        parts.append(str(args.bot_skill))
                    if args.bot_team is not None:
                        # Skill is positional before team.  If team is set
                        # but skill isn't, insert the engine default (4) so
                        # the team token lands in the right argv slot.
                        if args.bot_skill is None:
                            parts.append('4')
                        parts.append(args.bot_team)
                    addbot_cmd = ' '.join(parts)
                    resp2 = rcon_with_retry(actual_port, rcon_password, addbot_cmd)
                    bot_added = True
                    print(f'{tag} rcon: probe ok on port {actual_port} in '
                          f'{now - server_ready_at:.1f}s; sent '
                          f'{addbot_cmd!r}; reply '
                          f'{(resp2 or "<none>").strip()!r}')
    finally:
        kill_server(proc)
        reader.join(timeout=2.0)
        try:
            log_fp.close()
        except Exception:
            pass

    elapsed_total = time.monotonic() - started_at
    elapsed_from_ready = (
        cap_at - server_ready_at
        if cap_at and server_ready_at else None
    )

    # proc.returncode after our terminate/kill: distinguishes clean exit (0)
    # from an OS-killed crash (negative on POSIX, large positive on Win32 for
    # access violations and the like).  Useful when result is EXIT — a
    # returncode of 0 with no [CAP] line usually means the server cleanly
    # shut down on us (intermission + no nextmap, e.g.), while a non-zero
    # code points at an unhandled exception we missed with CRASH_RE.
    exit_code = proc.returncode if proc.returncode is not None else ''

    if cap_at:
        result = 'CAP'
    elif crashed_line:
        result = 'CRASH'
    elif timed_out:
        result = 'TIMEOUT'
    else:
        result = 'EXIT'

    return {
        'map':           mapname,
        'result':        result,
        'time_to_cap_s': round(elapsed_from_ready, 2) if elapsed_from_ready else '',
        'wall_total_s':  round(elapsed_total, 2),
        'cap_team':      cap_info['team']     if cap_info else '',
        'cap_capturer':  cap_info['capturer'] if cap_info else '',
        'exit_code':     exit_code,
        'crash_line':    crashed_line or '',
        'log_path':      str(log_path),
    }


# ---- entrypoint -------------------------------------------------------------

def main() -> None:
    ap = argparse.ArgumentParser(
        description='Bot CTF map smoke-test runner (drives openjkded.x86.exe).')

    # Process / asset paths
    ap.add_argument('--jka-base', default=DEFAULT_BASE,
                    help='JKA GameData folder (must contain base/). '
                         'Falls back to the JKA_GAMEDATA environment variable; '
                         'required if neither is set.')
    ap.add_argument('--bin', default=DEFAULT_BIN,
                    help=f'Path to openjkded.x86.exe. Defaults to this repo\'s '
                         f'build\\Release output: {DEFAULT_BIN}')

    # Test parameters
    ap.add_argument('--timeout', type=int, default=120,
                    help='Per-map timeout in seconds (default 120).')
    ap.add_argument('--warmup-grace', type=int, default=15,
                    help='Extra seconds beyond --timeout before hard-killing '
                         'a hung server.')
    ap.add_argument('--bot-add-delay', type=float, default=2.0,
                    help='Seconds to wait after "Server Initialization" '
                         'before sending kickall/addbot via rcon.')
    ap.add_argument('--gametype', type=int, default=8,
                    help='g_gametype (8 = CTF, 9 = CTY). Default 8.')
    ap.add_argument('--telemetry', type=int, default=0,
                    help='bot_telemetry bitmask (1 = movement, 2 = combat, '
                         '3 = both). Default 0 because the per-frame STATE '
                         'Transition spam buries everything else; pass 1 '
                         'when investigating a single map.')

    # Bot identity.  Skill and team are intentionally optional with no
    # default — when omitted, the rcon command is just `addbot <name>` and
    # the engine picks a sensible default skill (4) and auto-assigns the
    # under-populated team.  See Svcmd_AddBot_f in codemp/game/g_bot.c.
    ap.add_argument('--bot-name',  default=DEFAULT_BOT_NAME,
                    help=f'Bot to addbot (must exist in botfiles/bots.txt). '
                         f'Default: {DEFAULT_BOT_NAME}')
    ap.add_argument('--bot-skill', type=int, default=None,
                    help='Bot skill (1-5). Omit to use the engine default.')
    ap.add_argument('--bot-team',  default=None,
                    help='Bot team (r/b/free/spec). Omit to let the server '
                         'auto-pick the under-populated team.')

    # Run shape
    ap.add_argument('--map', action='append',
                    help='Test only this map (repeatable).  Use "mp/ctf_xxx" form.')
    ap.add_argument('--list', action='store_true',
                    help='Discover CTF maps from pk3s, print them, and exit. '
                         'Use this first to see what map names are valid on '
                         'your install before passing --map.')
    ap.add_argument('--limit', type=int, default=0,
                    help='Stop after N discovered maps (0 = all).')
    ap.add_argument('--parallel', type=int, default=1,
                    help='Run this many maps concurrently.  Each worker gets a '
                         'unique net_port and fs_homepath.  Default 1 (serial).')
    ap.add_argument('--port-base', type=int, default=DEFAULT_PORT_BASE,
                    help=f'First worker uses this UDP port; subsequent '
                         f'workers use +1, +2, ... Default {DEFAULT_PORT_BASE}.')

    # Output
    ap.add_argument('--output', default='bot_test_results.csv',
                    help='CSV results path.')
    ap.add_argument('--log-dir', default='bot_test_logs',
                    help='Directory for per-map server logs and worker '
                         'homepaths.')
    ap.add_argument('--quiet', action='store_true',
                    help='Do not echo server stdout to terminal '
                         '(per-map .log files always written).')
    ap.add_argument('--verbose-cmd', action='store_true',
                    help='Print the full command line before each launch.')

    args = ap.parse_args()

    if not args.jka_base:
        sys.exit('ERROR: JKA GameData path not set. '
                 'Pass --jka-base, or set the JKA_GAMEDATA environment variable '
                 'to the absolute path of your "GameData" folder.')
    base = Path(args.jka_base)
    if not (base / 'base').is_dir():
        sys.exit(f'ERROR: {base / "base"} not found. '
                 f'Check --jka-base / $JKA_GAMEDATA points at your JKA '
                 f'GameData folder.')
    if not Path(args.bin).is_file():
        sys.exit(f'ERROR: dedicated binary not found: {args.bin}\n'
                 f'Build the OpenJKDed target in Visual Studio (Release config) '
                 f'or pass --bin <path>.')

    Path(args.log_dir).mkdir(parents=True, exist_ok=True)

    # --list: discover maps and exit without launching anything.
    if args.list:
        discovered = discover_ctf_maps(base)
        print(f'[runner] Discovered {len(discovered)} CTF maps under '
              f'{base / "base"}:')
        for m in discovered:
            print(f'  {m}')
        return

    discovered = discover_ctf_maps(base)
    if args.map:
        maps = list(args.map)
        # Warn (don't abort) if the user typed a name that wasn't in the
        # discovery — useful when testing a map that lives in a non-standard
        # path inside a pk3, or when we just want to confirm a typo before
        # the dedicated server fails opaquely.
        unknown = [m for m in maps if m not in discovered]
        if unknown:
            print(f'[runner] WARNING: these --map names were NOT found by '
                  f'pk3 discovery (typo? non-standard path?): {unknown}')
            print(f'[runner]          Run with --list to see what is '
                  f'available on this install.')
        print(f'[runner] Testing user-supplied maps: {maps}')
    else:
        maps = discovered
        print(f'[runner] Discovered {len(maps)} CTF maps under '
              f'{base / "base"}.')
        if args.limit > 0:
            maps = maps[:args.limit]
            print(f'[runner] Limiting to first {args.limit}.')
    if not maps:
        sys.exit('ERROR: no maps to test.')

    print(f'[runner] Starting at {datetime.now().isoformat(timespec="seconds")}')
    print(f'[runner] Output CSV : {args.output}')
    print(f'[runner] Log dir    : {args.log_dir}')
    print(f'[runner] Parallelism: {args.parallel}')

    fieldnames = ['map', 'result', 'time_to_cap_s', 'wall_total_s',
                  'cap_team', 'cap_capturer', 'exit_code', 'crash_line',
                  'log_path']

    out_path = Path(args.output)
    results: list[dict] = []
    csv_lock = threading.Lock()

    with out_path.open('w', newline='', encoding='utf-8') as csv_f:
        writer = csv.DictWriter(csv_f, fieldnames=fieldnames)
        writer.writeheader()
        csv_f.flush()

        def worker(idx_map: tuple[int, str]) -> dict:
            worker_idx, mapname = idx_map
            try:
                row = run_one_map(args, mapname, worker_idx)
            except Exception as e:
                row = {'map': mapname, 'result': f'RUNNER_ERROR: {e!r}',
                       'time_to_cap_s': '', 'wall_total_s': '',
                       'cap_team': '', 'cap_capturer': '', 'exit_code': '',
                       'crash_line': '', 'log_path': ''}
            with csv_lock:
                writer.writerow(row)
                csv_f.flush()
                results.append(row)
                print(f'[runner] {row["map"]:<32} -> {row["result"]:<8} '
                      f'(cap {row["time_to_cap_s"]}s, '
                      f'wall {row["wall_total_s"]}s)')
            return row

        try:
            if args.parallel <= 1:
                # Serial: reuse worker index 0 for every map so we always grab
                # port_base unless the user changed it.
                for m in maps:
                    worker((0, m))
            else:
                # Parallel: spawn N long-lived worker threads, each pinned to a
                # fixed worker_idx (and therefore fixed net_port + homepath) for
                # its entire lifetime.  Maps are pulled off a shared FIFO queue.
                #
                # We do NOT use ThreadPoolExecutor with `worker_idx = i % N`:
                # if one map finishes faster than another, the pool can hand the
                # next task to a worker thread different from the one whose idx
                # would naturally cycle into that slot — producing a port
                # collision with a still-running server.  A pinned worker
                # makes that impossible by construction.
                work_q: 'queue.Queue[Optional[str]]' = queue.Queue()
                for m in maps:
                    work_q.put(m)
                for _ in range(args.parallel):
                    work_q.put(None)  # one poison pill per worker

                def worker_loop(worker_idx: int) -> None:
                    while True:
                        m = work_q.get()
                        if m is None:
                            return
                        worker((worker_idx, m))

                threads = [
                    threading.Thread(target=worker_loop, args=(i,),
                                     name=f'bot-test-W{i}', daemon=True)
                    for i in range(args.parallel)
                ]
                for t in threads:
                    t.start()
                for t in threads:
                    t.join()
        except KeyboardInterrupt:
            print('\n[runner] Interrupted by user.')

    n_cap = sum(1 for r in results if r['result'] == 'CAP')
    n_to  = sum(1 for r in results if r['result'] == 'TIMEOUT')
    n_xx  = len(results) - n_cap - n_to
    print(f'[runner] DONE: {n_cap} CAP, {n_to} TIMEOUT, {n_xx} other.  '
          f'({out_path})')


if __name__ == '__main__':
    main()
