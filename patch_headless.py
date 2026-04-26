import re

with open('codemp/game/ai_bot2_nav.c', 'r') as f:
    content = f.read()

start_idx = content.find('void Bot2_ScanWallruns(')
end_idx = content.find('// ==============================================================================\n// Wallrun Surface Checker', start_idx)

if start_idx == -1 or end_idx == -1:
    print("Could not find function bounds")
    exit(1)

func_str = content[start_idx:end_idx]

# Replace function name
func_str = func_str.replace('void Bot2_ScanWallruns(int gridStep, float radius, int centerClient)', 'void Bot2_ScanWallrunsHeadless(int gridStep, float radius, int centerClient)')

# Replace templatePS logic
template_logic_old = """	const playerState_t *templatePS = NULL;
	int tracePass = 0;
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (g_entities[i].inuse && g_entities[i].client) {
			templatePS = &g_entities[i].client->ps;
			tracePass  = i;
			break;
		}
	}
	if (!templatePS) {
		trap->Print("[WR-SCAN] ERROR: no connected client — need at least one player in-game to borrow a PS template.\\n");
		free(cons);
		return;
	}
	trap->Print("[WR-SCAN] Using client %d as PS template.\\n", tracePass);"""

template_logic_new = """	playerState_t dummyPS;
	memset(&dummyPS, 0, sizeof(dummyPS));
	dummyPS.clientNum = 0;
	dummyPS.weapon = WP_SABER;
	dummyPS.stats[STAT_WEAPONS] = (1 << WP_SABER);
	dummyPS.fd.forcePowerLevel[FP_LEVITATION] = FORCE_LEVEL_3;
	dummyPS.fd.forcePowersKnown = (1 << FP_LEVITATION);
	dummyPS.groundEntityNum = ENTITYNUM_NONE;
	const playerState_t *templatePS = &dummyPS;
	int tracePass = ENTITYNUM_NONE;
	trap->Print("[WR-SCAN-HEADLESS] Using synthetic PS template.\\n");"""

func_str = func_str.replace(template_logic_old, template_logic_new)

# Add NavMesh Pre-Filtering before probe loop
prefilter_old = """			// Bot stands with feet at groundTr.endpos.
			vec3_t groundPos;
			VectorCopy(groundTr.endpos, groundPos);

			// Probe horizontally in WALLRUN_SCAN_PROBE_DIRS directions."""

prefilter_new = """			// Bot stands with feet at groundTr.endpos.
			vec3_t groundPos;
			VectorCopy(groundTr.endpos, groundPos);

			// NavMesh Pre-Filtering: if the floor itself isn't on the navmesh, skip 16 probes!
			vec3_t testGround;
			VectorCopy(groundPos, testGround);
			testGround[2] += 2.0f;
			if (!NavMesh_IsPointOnMesh(testGround)) { floorFiltered++; continue; }

			// Probe horizontally in WALLRUN_SCAN_PROBE_DIRS directions."""

func_str = func_str.replace(prefilter_old, prefilter_new)

# Pre-simulation deduplication
dedup_old = """				// ---- Run full scenario sweep from flush position ----
				vec3_t landPos = { 0, 0, 0 };
				vec3_t zeroVel = { 0, 0, 0 };"""

dedup_new = """				// ---- Pre-Simulation Deduplication ----
				qboolean dupStart = qfalse;
				for (int c = 0; c < conCount; c++) {
					vec3_t ds;
					VectorSubtract(cons[c].start, simStart, ds);
					if (DotProduct(ds, ds) < 16.0f*16.0f) {
						dupStart = qtrue;
						break;
					}
				}
				if (dupStart) continue;

				// ---- Run full scenario sweep from flush position ----
				vec3_t landPos = { 0, 0, 0 };
				vec3_t zeroVel = { 0, 0, 0 };"""

func_str = func_str.replace(dedup_old, dedup_new)

# Replace the output sidecar file name so we can distinguish it (e.g. .nav_connections_headless)
func_str = func_str.replace('maps/%s.nav_connections', 'maps/%s.nav_connections_headless')
func_str = func_str.replace('bot_scan_wallruns', 'bot_scan_wallruns_headless')

# Inject into original file
new_content = content[:end_idx] + func_str + content[end_idx:]

with open('codemp/game/ai_bot2_nav.c', 'w') as f:
    f.write(new_content)

print("Injected Bot2_ScanWallrunsHeadless successfully.")
