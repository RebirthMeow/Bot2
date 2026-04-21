import struct
import sys
import os

def get_node_coords(filepath, node_ids):
    if not os.path.exists(filepath):
        return [f"File not found: {filepath}"]

    results = {}
    with open(filepath, "rb") as f:
        f.read(4) # magic
        f.read(4) # version
        num_nodes = struct.unpack("i", f.read(4))[0]
        
        for i in range(num_nodes):
            node_id = struct.unpack("i", f.read(4))[0]
            origin = struct.unpack("fff", f.read(12))
            num_edges = struct.unpack("i", f.read(4))[0]
            
            if node_id in node_ids:
                results[node_id] = origin
            
            # Skip edges to get to next node
            f.seek(num_edges * 24, 1)

    output = []
    for nid in node_ids:
        coords = results.get(nid, "Not Found")
        output.append(f"Node {nid}: {coords}")
    return output

if __name__ == "__main__":
    path = "C:\\Users\\Ryan\\Documents\\My Games\\OpenJK\\base\\maps\\mp\\ctf_kejim.act"
    # 1249 (Start), 550 (Goal), 2140 (Gap Side A), 2089 (Gap Side B)
    res = get_node_coords(path, [1249, 550, 2140, 2089])
    with open("node_coords.txt", "w") as f:
        f.write("\n".join(res))
