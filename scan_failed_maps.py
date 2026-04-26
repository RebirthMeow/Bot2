"""
Re-runs the maps that previously failed.
Shares the same categorisation logic as scan_all_maps.py.
"""
import os
import sys
import subprocess
import time

from scan_all_maps import (
    GAME_DATA, NAV_EXTS,
    FAIL_CRASH_SHUTDOWN, FAIL_NO_NAVMESH, FAIL_NO_SPAWN,
    FAIL_LOAD_CRASH, FAIL_TIMEOUT, FAIL_UNKNOWN,
    DESCRIPTIONS,
    nav_file_path, categorize, print_progress, print_issue,
)

FAILED_MAPS = [
    "academy5",
    "ctf_dantooine_v3",
    "ctf_emerido_v2",
    "duel_lego",
    "hoth2",
    "kor1",
    "mp/ctf1",
    "mp/ctf1_pro4",
    "mp/ctf2",
    "mp/ctf5",
    "mp/ctf_anoat",
    "mp/ctf_belgaroth",
    "mp/ctf_bryndar",
    "mp/ctf_byss",
    "mp/ctf_cairn",
    "mp/ctf_carratos",
    "mp/ctf_chromovon",
    "mp/ctf_dash2",
    "mp/ctf_denon",
    "mp/ctf_elrood",
    "mp/ctf_emerido",
    "mp/ctf_hor",
    "mp/ctf_ichtor",
    "mp/ctf_illimiran",
    "mp/ctf_ilum",
    "mp/ctf_kejim",
    "mp/ctf_korriban",
    "mp/ctf_kothis_v4",
    "mp/ctf_metalorn",
    "mp/ctf_nelvaan",
    "mp/ctf_paulking",
    "mp/ctf_reboam",
    "mp/ctf_rishi_alpha",
    "mp/ctf_sikurd",
    "mp/ctf_talay",
    "mp/ctf_taloraan",
    "mp/ctf_talravin",
    "mp/duel10",
    "mp/ffa3",
    "mp/siege_korriban",
    "t1_danger",
    "t1_fatal",
    "t2_rogue",
    "t2_wedge",
    "t3_byss",
    "taspir1",
    "taspir2",
    "vjun1",
    "vjun3",
    "yavin1",
    "yavin2",
]

def main():
    exe_path = os.path.join(GAME_DATA, "openjkded.x86.exe")
    if not os.path.exists(exe_path):
        print(f"Executable not found at {exe_path}")
        return

    maps = FAILED_MAPS
    print(f"Re-running {len(maps)} previously-failed maps...")
    print_progress(0, len(maps), prefix='Progress:', suffix='Starting…')

    start_time = time.time()
    issues = []

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
        category = None

        try:
            proc = subprocess.run(
                cmd, cwd=GAME_DATA,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=True, timeout=120
            )
            stdout, stderr = proc.stdout, proc.stderr
            category = categorize(map_name, proc.returncode, stdout, stderr)

        except subprocess.TimeoutExpired:
            category = FAIL_TIMEOUT
        except Exception as e:
            category = FAIL_UNKNOWN
            stderr = str(e)

        if category is not None:
            issues.append((map_name, category, stdout, stderr))
            if category != FAIL_CRASH_SHUTDOWN:
                print_issue(map_name, category)

        print_progress(i + 1, len(maps),
                       prefix='Progress:',
                       suffix=f'({map_name})')

    elapsed = time.time() - start_time
    print(f"\nDone in {elapsed:.1f} s\n")

    genuine  = [(m, c, o, e) for m, c, o, e in issues if c != FAIL_CRASH_SHUTDOWN]
    cosmetic = [(m, c, o, e) for m, c, o, e in issues if c == FAIL_CRASH_SHUTDOWN]

    if cosmetic:
        print(f"  {len(cosmetic)} maps crashed on shutdown but scan files are intact:")
        for m, c, _, _ in cosmetic:
            print(f"    {m}")

    if genuine:
        print(f"\n  {len(genuine)} maps with genuine issues:")
        for m, c, _, _ in genuine:
            print(f"    [{c}]  {m}")

    if not issues:
        print("  All maps passed!")

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
