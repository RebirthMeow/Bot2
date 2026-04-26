import os
import subprocess
import time
import glob

game_data_dir = r"D:\ACodingBot\testplace\Jedi Academy\GameData"
exe_path = os.path.join(game_data_dir, "openjkded.x86.exe")

cmd = [
    exe_path,
    "+set", "dedicated", "2",
    "+map", "mp/ctf4",
    "+addbot", "alora", "1", "blue",
    "+wait", "100",
    "+bot_scan_wallruns_headless",
    "+quit"
]

# Clean any stale output from previous test runs
home_base = os.path.expanduser(r"~\Documents\My Games\OpenJK\base\maps")
if os.path.exists(home_base):
    for root, dirs, files in os.walk(home_base):
        for f in files:
            if 'ctf4' in f and 'nav_connections' in f:
                try:
                    os.remove(os.path.join(root, f))
                    print(f"Cleaned: {f}")
                except Exception as e:
                    print(f"Failed to clean {f}: {e}")

print("Running headless wallrun scan on mp/ctf4...")
start = time.time()
process = subprocess.run(cmd, cwd=game_data_dir, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=120)
elapsed = time.time() - start
print(f"Done in {elapsed:.1f}s  (exit code {process.returncode})")

# Find the output file (written to user Documents or GameData/base/maps)
search_paths = [
    os.path.expanduser(r"~\Documents\My Games\OpenJK\base\maps\mp\ctf4.nav_connections"),
    os.path.expanduser(r"~\Documents\My Games\OpenJK\base\maps\mp\ctf4.nav_connections_headless"),
    os.path.join(game_data_dir, r"base\maps\mp\ctf4.nav_connections"),
    os.path.join(game_data_dir, r"base\maps\mp\ctf4.nav_connections_headless"),
]

found = False
for path in search_paths:
    if os.path.exists(path):
        with open(path, 'r', errors='replace') as f:
            lines = f.readlines()
        print(f"\nOutput file: {path}")
        print(f"Line count:  {len(lines)}")
        found = True
        break

if not found:
    # Broader search
    for root, dirs, files in os.walk(os.path.expanduser(r"~\Documents\My Games\OpenJK")):
        for f in files:
            if 'ctf4' in f and 'nav_connections' in f:
                path = os.path.join(root, f)
                with open(path, 'r', errors='replace') as fh:
                    lines = fh.readlines()
                print(f"\nOutput file: {path}")
                print(f"Line count:  {len(lines)}")
                found = True

if not found:
    print("\nNo .nav_connections output file found.")
    print("stdout:", process.stdout[-2000:] if process.stdout else "(empty)")
    print("stderr:", process.stderr[-500:] if process.stderr else "(empty)")
