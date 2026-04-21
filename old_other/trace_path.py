"""
trace_path.py - Dumps all nodes in the ramp/transition area and shows
which graph component each belongs to, then reports the exact missing link.

Run from your OpenJK directory:
    python trace_path.py

Regions checked (from map geometry):
  Lower floor:   X[3008-3384], Y[4480-5440], Z[1300-1500]
  Ramp:          X[3008-3384], Y[4480-5440], Z[1500-1750]
  Ramp flat top: X[3008-3384], Y[5440-5800], Z[1700-1780]
  Corner slope:  X[2752-3008], Y[5440-5800], Z[1730-1930]
  Upper floor:   X[2176-2944], Y[4288-5500], Z[1700-1900]
"""
import struct, math, os
from collections import defaultdict

ACT_PATH = r"/sessions/optimistic-amazing-shannon/mnt/OpenJK/ctf_kejim.act"

# Flag positions from map entities - used to auto-locate start/goal nodes
RED_FLAG_POS  = (152,  -1160, 1920)   # team_CTF_redflag  origin
BLUE_FLAG_POS = (2464,  5312, 1920)   # team_CTF_blueflag origin

def load_act(path):
    nodes = []
    with open(path, "rb") as f:
        magic, version, num_nodes = struct.unpack("<iii", f.read(12))
        assert magic == 0x424F5432, f"Bad magic: 0x{magic:08X}"
        for _ in range(num_nodes):
            hdr = f.read(20)
            if len(hdr) < 20:
                print(f"  [load_act] File truncated at node {len(nodes)} (header short-read). Using {len(nodes)} nodes.")
                break
            nid, ox, oy, oz, ne = struct.unpack("<ifffi", hdr)
            edges = []
            ok = True
            for _ in range(ne):
                # actionEdge_t: int targetNodeId, float minSpeed, float entryYaw,
                #               int strafeDir, signed char upmove, [3 pad], int costTimeMs
                raw = f.read(24)
                if len(raw) < 24:
                    print(f"  [load_act] File truncated mid-edges at node {len(nodes)} (id={nid}). Using {len(nodes)} nodes.")
                    ok = False
                    break
                tid, mspd, eyw, sdir_raw, upmove_raw, cost = struct.unpack("<iffib3xi", raw)
                edges.append(tid)
            nodes.append({"id": nid, "o": (ox, oy, oz), "edges": edges})
            if not ok:
                break
    return nodes

def bfs(nodes, start_ids):
    visited = set(start_ids)
    queue = list(start_ids)
    head = 0
    while head < len(queue):
        u = queue[head]; head += 1
        for v in nodes[u]["edges"]:
            if 0 <= v < len(nodes) and v not in visited:
                visited.add(v); queue.append(v)
    return visited

def bfs_reverse(nodes, start_ids):
    rev = defaultdict(list)
    for u, n in enumerate(nodes):
        for v in n["edges"]:
            if 0 <= v < len(nodes):
                rev[v].append(u)
    visited = set(start_ids)
    queue = list(start_ids)
    head = 0
    while head < len(queue):
        v = queue[head]; head += 1
        for u in rev[v]:
            if u not in visited:
                visited.add(u); queue.append(u)
    return visited

def in_box(o, xr, yr, zr):
    return xr[0] <= o[0] <= xr[1] and yr[0] <= o[1] <= yr[1] and zr[0] <= o[2] <= zr[1]

REGIONS = {
    "LOWER_FLOOR":   ((3008,3384), (4480,5440), (1300,1500)),
    "RAMP":          ((3008,3384), (4480,5440), (1500,1750)),
    "RAMP_FLAT_TOP": ((3008,3384), (5440,5800), (1700,1780)),
    "CORNER_SLOPE":  ((2752,3008), (5440,5800), (1730,1930)),
    "UPPER_FLOOR":   ((2176,2944), (4288,5500), (1700,1900)),
}

def nearest_node(nodes, pos):
    best_id, best_d = -1, 1e18
    for n in nodes:
        d = math.dist(n["o"], pos)
        if d < best_d:
            best_d = d; best_id = n["id"]
    return best_id, best_d

print(f"Loading {ACT_PATH}...")
nodes = load_act(ACT_PATH)
print(f"Loaded {len(nodes)} nodes.\n")

# Auto-locate start/goal from flag positions so node IDs don't need manual updating
START_NODE, sd = nearest_node(nodes, RED_FLAG_POS)
GOAL_NODE,  gd = nearest_node(nodes, BLUE_FLAG_POS)
print(f"Red  flag nearest node:  {START_NODE}  (dist {sd:.1f})")
print(f"Blue flag nearest node:  {GOAL_NODE}   (dist {gd:.1f})\n")

reachable   = bfs(nodes, [START_NODE])
can_reach   = bfs_reverse(nodes, [GOAL_NODE])

print(f"Reachable from {START_NODE}: {len(reachable)} nodes")
print(f"Can reach {GOAL_NODE}:       {len(can_reach)} nodes")
print(f"Goal reachable from start? {'YES' if GOAL_NODE in reachable else 'NO'}\n")

print("=== Nodes in each ramp/transition region ===")
for region, (xr, yr, zr) in REGIONS.items():
    found = [n for n in nodes if in_box(n["o"], xr, yr, zr)]
    print(f"\n  [{region}]  ({len(found)} nodes)")
    for n in found:
        o = n["o"]
        r = "REACH" if n["id"] in reachable else "     "
        c = "CAN_REACH_GOAL" if n["id"] in can_reach else "              "
        ne = len(n["edges"])
        print(f"    Node {n['id']:5d} ({o[0]:7.1f},{o[1]:7.1f},{o[2]:7.1f})  {r}  {c}  edges={ne}")

# Find closest cross-component pair among these regions
print("\n=== Closest gap across region boundaries ===")
ramp_side   = [n for n in nodes if n["id"] in reachable and not n["id"] in can_reach
               and any(in_box(n["o"], xr, yr, zr) for (xr,yr,zr) in REGIONS.values())]
upper_side  = [n for n in nodes if n["id"] in can_reach and not n["id"] in reachable
               and any(in_box(n["o"], xr, yr, zr) for (xr,yr,zr) in REGIONS.values())]

best_d, best_pair = 1e9, (None, None)
for a in ramp_side:
    for b in upper_side:
        d = math.dist(a["o"], b["o"])
        if d < best_d:
            best_d = d; best_pair = (a, b)

if best_pair[0]:
    a, b = best_pair
    dz = b["o"][2] - a["o"][2]
    dxy = math.dist(a["o"][:2], b["o"][:2])
    print(f"  Closest ramp-side node: {a['id']} at {a['o']}")
    print(f"  Closest upper-side node: {b['id']} at {b['o']}")
    print(f"  3D distance: {best_d:.1f}  |  Horizontal: {dxy:.1f}  |  dZ: {dz:.1f}")
    if dz > 320:
        print("  -> Height delta too large for Force Jump 3 to bridge directly.")
    if best_d > 512:
        print("  -> 3D distance exceeds the 512-unit bake radius: no edge was ever attempted.")
    elif dz > 0 and dxy < 200:
        print("  -> Near-vertical gap: likely a step/ledge the walk sim fails on.")
    else:
        print("  -> Within bake radius. Check walk sim ground-loss failure on slope transition.")
else:
    print("  No cross-component nodes found in these regions.")
    print("  The disconnect is outside the defined regions - widen the search boxes.")
