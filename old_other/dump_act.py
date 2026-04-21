import os
import struct
import sys

def find_act_file(filename):
    # Check current dir
    paths_to_check = [
        os.path.join("base", "maps", filename),
        os.path.join("base", "maps", "mp", filename),
        os.path.join(os.environ.get("USERPROFILE", ""), "Documents", "My Games", "OpenJK", "base", "maps", filename),
        os.path.join(os.environ.get("USERPROFILE", ""), "Documents", "My Games", "OpenJK", "base", "maps", "mp", filename),
        os.path.join("build", "base", "maps", filename),
        os.path.join("build", "base", "maps", "mp", filename),
    ]
    for p in paths_to_check:
        if os.path.exists(p):
            return p
    return None

def read_act_file(filepath):
    print(f"Reading {filepath}...\n")
    with open(filepath, "rb") as f:
        magic, version, num_nodes = struct.unpack("<iii", f.read(12))
        
        # Check Magic (BOT2 = 0x424F5432)
        print(f"Header Info:")
        print(f"  Magic: 0x{magic:08X}")
        print(f"  Version: {version}")
        print(f"  Total Nodes: {num_nodes}\n")
        
        if magic != 0x424F5432:
            print("ERROR: Magic number mismatch!")
            return

        total_edges = 0
        print("First 3 Nodes in File:")
        for i in range(num_nodes):
            nid, ox, oy, oz, num_edges = struct.unpack("<ifffi", f.read(20))
            total_edges += num_edges
            
            if i < 3:
                print(f"  Node {nid} @ ({ox:.1f}, {oy:.1f}, {oz:.1f}) | Edges: {num_edges}")
            
            # Read edges
            for e in range(num_edges):
                edge_data = struct.unpack("<ifffffii", f.read(32))
                target, minSpd, maxSpd, enYaw, yawTol, aimYaw, sDir, cost = edge_data
                if i < 3 and e < 2:  # Print first 2 edges of the first 3 nodes
                    dir_str = "Forward" if sDir == 0 else ("W+D" if sDir == 1 else ("W+A" if sDir == -1 else ("Just D" if sDir == 2 else "Just A")))
                    print(f"    -> Edge to Node {target} | Spd: {minSpd:.0f}-{maxSpd:.0f} | Yaw: {enYaw:.0f} (+/-{yawTol:.0f}) | Cost: {cost}ms | Keys: {dir_str}")
                if i < 3 and e == 2 and num_edges > 2:
                    print(f"    -> ... ({num_edges - 2} more edges)")
            if i == 2 and num_nodes > 3:
                print("\n  ...\n")
                
        print(f"Successfully parsed {num_nodes} Nodes and {total_edges} Total Edges!")

if __name__ == "__main__":
    filepath = find_act_file("ctf_kejim.act")
    if not filepath:
        print("Could not find ctf_kejim.act in standard OpenJK directories.")
        print("If you saved the file to a different game directory, please specify it.")
        sys.exit(1)
        
    read_act_file(filepath)
