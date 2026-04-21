import struct
import sys
import os
import math
from collections import defaultdict

def analyze_gap(filepath, start_node_id, end_node_id):
    output_lines = []
    if not os.path.exists(filepath):
        return [f"File not found: {filepath}"]

    with open(filepath, "rb") as f:
        f.read(4) # magic
        f.read(4) # version
        num_nodes = struct.unpack("i", f.read(4))[0]
        
        nodes = []
        reverse_adj = defaultdict(list)
        for i in range(num_nodes):
            node_id = struct.unpack("i", f.read(4))[0]
            origin = struct.unpack("fff", f.read(12))
            num_edges = struct.unpack("i", f.read(4))[0]
            edges = []
            for j in range(num_edges):
                unpacked = struct.unpack("ifffb3xi", f.read(24))
                target = unpacked[0]
                edges.append({"target": target})
                if 0 <= target < num_nodes:
                    reverse_adj[target].append(i)
            nodes.append({"id": node_id, "origin": origin, "edges": edges})

    # 1. Nodes reachable FROM start_node_id
    reachable_from_start = set()
    queue = [start_node_id]
    reachable_from_start.add(start_node_id)
    head = 0
    while head < len(queue):
        u = queue[head]
        head += 1
        for edge in nodes[u]["edges"]:
            v = edge["target"]
            if 0 <= v < num_nodes and v not in reachable_from_start:
                reachable_from_start.add(v)
                queue.append(v)

    # 2. Nodes that CAN REACH end_node_id (BFS on reverse graph)
    can_reach_end = set()
    queue = [end_node_id]
    can_reach_end.add(end_node_id)
    head = 0
    while head < len(queue):
        v = queue[head]
        head += 1
        for u in reverse_adj[v]:
            if u not in can_reach_end:
                can_reach_end.add(u)
                queue.append(u)

    output_lines.append(f"Analysis: Path from Node {start_node_id} to Node {end_node_id}")
    output_lines.append(f"Nodes reachable from {start_node_id}: {len(reachable_from_start)}")
    output_lines.append(f"Nodes that can reach {end_node_id}: {len(can_reach_end)}")

    if end_node_id in reachable_from_start:
        output_lines.append(f"SUCCESS: Node {end_node_id} is already reachable from Node {start_node_id}!")
        return output_lines

    output_lines.append("\n--- GAP DETECTION ---")
    output_lines.append(f"The graph is split. Searching for the shortest jump to connect the two islands...")

    # Find the closest pair between the two sets
    best_dist = 99999999
    best_pair = (-1, -1)

    # Spatial optimization for the search
    grid = defaultdict(list)
    GRID_SIZE = 512
    for idx in can_reach_end:
        pos = nodes[idx]["origin"]
        gx, gy = int(pos[0] // GRID_SIZE), int(pos[1] // GRID_SIZE)
        grid[(gx, gy)].append(idx)

    for i in reachable_from_start:
        pos_i = nodes[i]["origin"]
        gx, gy = int(pos_i[0] // GRID_SIZE), int(pos_i[1] // GRID_SIZE)
        for dx in range(-1, 2):
            for dy in range(-1, 2):
                for j in grid[(gx + dx, gy + dy)]:
                    pos_j = nodes[j]["origin"]
                    d = math.sqrt(sum((pos_i[k]-pos_j[k])**2 for k in range(3)))
                    if d < best_dist:
                        best_dist = d
                        best_pair = (i, j)

    if best_pair[0] != -1:
        i, j = best_pair
        output_lines.append(f"Closest Gap: Node {i} (Start-Island) -> Node {j} (End-Island)")
        output_lines.append(f"Distance: {best_dist:.1f}")
        output_lines.append(f"  Node {i} Origin: {nodes[i]['origin']}")
        output_lines.append(f"  Node {j} Origin: {nodes[j]['origin']}")
        
        # Check why it failed
        dz = nodes[j]['origin'][2] - nodes[i]['origin'][2]
        dx = nodes[j]['origin'][0] - nodes[i]['origin'][0]
        dy = nodes[j]['origin'][1] - nodes[i]['origin'][1]
        dist2d = math.sqrt(dx*dx + dy*dy)
        output_lines.append(f"  Physical Delta: 2D Dist: {dist2d:.1f}, Height Delta: {dz:.1f}")
        
        if dz > 320:
            output_lines.append("  REASON: Height delta exceeds Force Jump 3 limit (320 units).")
        elif dist2d > 512:
            output_lines.append("  REASON: Horizontal distance likely requires more momentum than currently baked.")
    else:
        output_lines.append("No nearby nodes found between the two sets.")

    return output_lines

if __name__ == "__main__":
    path = "C:\\Users\\Ryan\\Documents\\My Games\\OpenJK\\base\\maps\\mp\\ctf_kejim.act"
    res = analyze_gap(path, 1249, 550)
    with open("gap_analysis.txt", "w") as f:
        f.write("\n".join(res))
