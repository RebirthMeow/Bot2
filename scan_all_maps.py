import os
import sys
import zipfile
import subprocess
import time

# ── Output file locations ────────────────────────────────────────────────────
HOME_BASE    = os.path.expanduser(r"~\Documents\My Games\OpenJK\base")
GAME_DATA    = r"D:\ACodingBot\testplace\Jedi Academy\GameData"
NAV_EXTS     = ('.nav_connections_headless', '.nav_connections')

def nav_file_path(map_name):
    """Return the .nav_connections path if it exists on disk, else None."""
    for base in (HOME_BASE, os.path.join(GAME_DATA, "base")):
        for ext in NAV_EXTS:
            p = os.path.join(base, 'maps', map_name + ext)
            if os.path.exists(p):
                return p
    return None

# ── Failure categorisation ───────────────────────────────────────────────────
FAIL_CRASH_SHUTDOWN = "CRASH-SHUTDOWN"   # scan wrote file, crash on exit — data OK
FAIL_NO_NAVMESH     = "NO-NAVMESH"       # map has no navmesh, scan skipped
FAIL_NO_SPAWN       = "NO-SPAWN"         # bot couldn't find a spawn point
FAIL_LOAD_CRASH     = "LOAD-CRASH"       # crashed before scan even started
FAIL_TIMEOUT        = "TIMEOUT"
FAIL_UNKNOWN        = "UNKNOWN"

DESCRIPTIONS = {
    FAIL_CRASH_SHUTDOWN : "scan OK — crash on shutdown (file written, data safe)",
    FAIL_NO_NAVMESH     : "no navmesh loaded — map cannot be scanned",
    FAIL_NO_SPAWN       : "bot couldn't find a spawn point",
    FAIL_LOAD_CRASH     : "crashed before scan started (SP map or missing assets?)",
    FAIL_TIMEOUT        : "timed out after 120 s",
    FAIL_UNKNOWN        : "unknown failure",
}

def categorize(map_name, returncode, stdout, stderr):
    if returncode == 0:
        return None  # success — nothing to report

    combined = (stdout or '') + (stderr or '')

    if 'navmesh not loaded' in combined:
        return FAIL_NO_NAVMESH

    if "Couldn't find a spawn point" in combined:
        return FAIL_NO_SPAWN

    # Scan completed and wrote the file, then crashed during shutdown
    file_ok = nav_file_path(map_name) is not None
    wrote_line = '[WR-SCAN] Wrote' in combined
    shutdown_line = 'ShutdownGame' in combined
    if (wrote_line or file_ok) and shutdown_line:
        return FAIL_CRASH_SHUTDOWN

    # Never got past early startup
    if '[WR-SCAN]' not in combined and 'ClientBegin' not in combined:
        return FAIL_LOAD_CRASH

    return FAIL_UNKNOWN

# ── Helpers ──────────────────────────────────────────────────────────────────
def find_maps(base_path):
    maps = set()
    maps_dir = os.path.join(base_path, 'maps')
    if os.path.exists(maps_dir):
        for root, dirs, files in os.walk(maps_dir):
            for f in files:
                if f.lower().endswith('.bsp'):
                    rel_path = os.path.relpath(os.path.join(root, f), maps_dir)
                    maps.add(os.path.splitext(rel_path)[0].replace('\\', '/'))

    for root, dirs, files in os.walk(base_path):
        for f in files:
            if f.lower().endswith('.pk3'):
                try:
                    with zipfile.ZipFile(os.path.join(root, f), 'r') as z:
                        for name in z.namelist():
                            if name.lower().startswith('maps/') and name.lower().endswith('.bsp'):
                                maps.add(os.path.splitext(name)[0][5:])
                except Exception:
                    pass
    return sorted(maps)

def clean_old_scans():
    maps_dir = os.path.join(HOME_BASE, "maps")
    count = 0
    if os.path.exists(maps_dir):
        for root, dirs, files in os.walk(maps_dir):
            for f in files:
                if f.endswith('.nav_connections') or f.endswith('.nav_connections_headless'):
                    try:
                        os.remove(os.path.join(root, f))
                        count += 1
                    except Exception as e:
                        print(f"Failed to delete {f}: {e}")
    if count > 0:
        print(f"Cleaned up {count} old .nav_connections files to prevent duplication.")

def print_progress(iteration, total, prefix='', suffix='', length=50, fill='█'):
    percent = f"{100 * iteration / total:.1f}"
    filled  = int(length * iteration // total)
    bar     = fill * filled + '-' * (length - filled)
    sys.stdout.write('\033[2K\033[1G')
    sys.stdout.write(f'{prefix} |{bar}| {percent}% {suffix}')
    sys.stdout.flush()
    if iteration == total:
        sys.stdout.write('\n')

def print_issue(map_name, category):
    """Print a persistent failure line above the progress bar."""
    label = f"[{category}]"
    desc  = DESCRIPTIONS.get(category, '')
    # \n moves past the current progress bar line so it stays visible
    sys.stdout.write(f'\n  {label:<18} {map_name}  —  {desc}\n')
    sys.stdout.flush()

# ── Main ─────────────────────────────────────────────────────────────────────
def main():
    base_dir = os.path.join(GAME_DATA, "base")
    exe_path = os.path.join(GAME_DATA, "openjkded.x86.exe")

    if not os.path.exists(exe_path):
        print(f"Executable not found at {exe_path}")
        return

    print("Scanning for maps in GameData/base...")
    maps = find_maps(base_dir)
    print(f"Found {len(maps)} maps.")
    if not maps:
        return

    clean_old_scans()
    print("\nStarting headless wallrun scans...")
    print_progress(0, len(maps), prefix='Progress:', suffix='Starting…')

    start_time  = time.time()
    issues      = []   # (map_name, category)

    for i, map_name in enumerate(maps):
        cmd = [
            exe_path,
            "+set", "dedicated", "2",
            "+map", map_name,
            "+addbot", "alora", "1", "blue",
            "+wait", "100",
            "+bot_scan_wallruns_headless",
            "+quit"
        ]

        stdout = stderr = ''
        returncode = 0
        category   = None

        try:
            proc = subprocess.run(
                cmd, cwd=GAME_DATA,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, timeout=120
            )
            returncode = proc.returncode
            stdout, stderr = proc.stdout, proc.stderr
            category = categorize(map_name, returncode, stdout, stderr)

        except subprocess.TimeoutExpired:
            category = FAIL_TIMEOUT
        except Exception as e:
            category = FAIL_UNKNOWN
            stderr = str(e)

        if category is not None:
            issues.append((map_name, category, stdout, stderr))
            # Only print inline for genuine failures — not shutdown crashes
            # (those are noise; the data is fine)
            if category != FAIL_CRASH_SHUTDOWN:
                print_issue(map_name, category)

        print_progress(i + 1, len(maps),
                       prefix='Progress:',
                       suffix=f'({map_name})')

    elapsed = time.time() - start_time
    print(f"\nDone in {elapsed:.1f} s\n")

    # ── Summary ──────────────────────────────────────────────────────────────
    genuine   = [(m, c, o, e) for m, c, o, e in issues if c != FAIL_CRASH_SHUTDOWN]
    cosmetic  = [(m, c, o, e) for m, c, o, e in issues if c == FAIL_CRASH_SHUTDOWN]

    if cosmetic:
        print(f"  {len(cosmetic)} maps crashed on shutdown but their scan files are intact:")
        for m, c, _, _ in cosmetic:
            print(f"    {m}")

    if genuine:
        print(f"\n  {len(genuine)} maps with genuine issues:")
        for m, c, _, _ in genuine:
            print(f"    [{c}]  {m}")

    if not issues:
        print("  All maps scanned successfully!")

    # ── Log file ─────────────────────────────────────────────────────────────
    if issues:
        log_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "scan_failures.log")
        with open(log_path, 'w', encoding='utf-8', errors='replace') as log:
            log.write(f"Scan log — {len(issues)} issues\n")
            log.write("=" * 60 + "\n\n")
            for m, c, out, err in issues:
                log.write(f"MAP: {m}  [{c}]  {DESCRIPTIONS[c]}\n")
                log.write("-" * 40 + "\n")
                combined = (out or '') + (err or '')
                if combined.strip():
                    log.write(combined[-3000:])
                    log.write("\n")
                log.write("\n")
        print(f"\n  Full log: {log_path}")

if __name__ == '__main__':
    main()
