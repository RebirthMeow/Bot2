with open(r'C:\Users\Ryan\Documents\My Games\OpenJK\base\maps\mp\ctf4.nav_connections', 'r') as f:
    lines = f.readlines()

print(f"Total lines: {len(lines)}")

# The file was appended to, so if there was a header, it might be at the very top.
# Let's filter out any header lines (e.g. starting with '#' or non-data lines)
data_lines = [l.strip() for l in lines if l.strip() and not l.startswith('#')]

print(f"Total data lines: {len(data_lines)}")

if len(data_lines) % 2 != 0:
    print("Warning: Odd number of data lines, they can't be perfectly split in half.")

half = len(data_lines) // 2
part1 = data_lines[:half]
part2 = data_lines[half:]

if part1 == part2:
    print("SUCCESS: The 1,280 lines from the headless run are EXACTLY identical (in exact order) to the live run.")
else:
    print("Order differs or exact match failed. Checking sets...")
    set1 = set(part1)
    set2 = set(part2)
    
    diff1 = set1 - set2
    diff2 = set2 - set1
    
    if not diff1 and not diff2 and len(set1) == len(set2):
        print("SUCCESS: The sets of connections are identical, but they were written in a different order.")
    else:
        print(f"DIFFERENCE FOUND!")
        print(f"Connections in live run but not headless: {len(diff1)}")
        print(f"Connections in headless but not live run: {len(diff2)}")
        
        print("\nSample of missing in headless:")
        for i, d in enumerate(list(diff1)[:5]):
            print(f"  {d}")
            
        print("\nSample of extra in headless:")
        for i, d in enumerate(list(diff2)[:5]):
            print(f"  {d}")