import os

file1 = r'D:\ACodingBot\testplace\nav mesh generation\bsp\maps\mp\ctf4.nav_connections'
file2 = r'C:\Users\Ryan\Documents\My Games\OpenJK\base\maps\mp\ctf4.nav_connections'

def load_data(path):
    with open(path, 'r') as f:
        return [l.strip() for l in f.readlines() if l.strip() and not l.startswith('#')]

data1 = load_data(file1)
data2 = load_data(file2)

print(f"Reference file lines: {len(data1)}")
print(f"Generated file lines: {len(data2)}")

if data1 == data2:
    print("SUCCESS: The files are EXACTLY identical (in exact order).")
else:
    set1 = set(data1)
    set2 = set(data2)
    
    diff1 = set1 - set2
    diff2 = set2 - set1
    
    if not diff1 and not diff2 and len(set1) == len(set2):
        print("SUCCESS: The sets of connections are identical, but they were written in a different order.")
    else:
        print(f"DIFFERENCE FOUND!")
        print(f"Connections in Reference but not Generated: {len(diff1)}")
        print(f"Connections in Generated but not Reference: {len(diff2)}")
        
        print("\nSample of missing in Generated:")
        for i, d in enumerate(list(diff1)[:5]):
            print(f"  {d}")
            
        print("\nSample of extra in Generated:")
        for i, d in enumerate(list(diff2)[:5]):
            print(f"  {d}")