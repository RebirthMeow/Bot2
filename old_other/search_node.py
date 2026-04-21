import struct
import sys
import os
import math

def find_nearest_node(filepath, target_coords):
    if not os.path.exists(filepath):
        return [f"File not found: {filepath}"]

    best_dist = 99999999
    best_node = -1
    best_coords = None

    with open(filepath, "rb") as f:
        f.read(4) # magic
        f.read(4) # version
        num_nodes = struct.unpack("i", f.read(4))[0]
        
        for i in range(num_nodes):
            node_id = struct.unpack("i", f.read(4))[0]
            origin = struct.unpack("fff", f.read(12))
            num_edges = struct.unpack("i", f.read(4))[0]
            
            dist = math.sqrt(sum((origin[k] - target_coords[k])**2 for k in range(3)))
            if dist < best_dist:
                best_dist = dist
                best_node = node_id
                best_coords = origin
            
            f.seek(num_edges * 24, 1)

    return [
        f"Target Search: {target_coords}",
        f"Nearest Node: {best_node}",
        f"Coordinates: {best_coords}",
        f"Distance: {best_dist:.1f}"
    ]

if __name__ == "__main__":
    path = r"/sessions/optimistic-amazing-shannon/mnt/OpenJK/ctf_kejim.act"

    # Flag positions pulled from map entities (team_CTF_redflag / team_CTF_blueflag)
    targets = {
        "RED  flag (152, -1160, 1920)": (152,  -1160, 1920),
        "BLUE flag (2464, 5312, 1920)": (2464,  5312, 1920),
    }

    for label, target in targets.items():
        print(f"\n--- {label} ---")
        for line in find_nearest_node(path, target):
            print(line)
