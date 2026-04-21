import sys

with open('codemp/game/ai_bot2_nav.c', 'r') as f:
    content = f.read()

start_sig = 'qboolean IsSafeToJump(gentity_t* ent, int clientNum, vec3_t start, float current_speed, float vel_yaw, int testDir, float max_run_speed, char* failReason, char* warningString, vec3_t out_land_pos, float* out_land_speed) {'
start_idx = content.find(start_sig)

end_sig = 'void Bot2_GetLeadOrigin(gentity_t* ent, gentity_t* target, vec3_t out_leadPos) {'
end_idx = content.find(end_sig)

new_func = '''qboolean IsSafeToJump(gentity_t* ent, int clientNum, vec3_t start, float current_speed, float vel_yaw, int testDir, float max_run_speed, char* failReason, char* warningString, vec3_t out_land_pos, float* out_land_speed) {
	vec3_t pmove_land;
	
	if (!SimulatePmoveTrajectory(ent, vel_yaw, testDir, max_run_speed, pmove_land)) {
		if (failReason) Q_strncpyz(failReason, "Phantom Pmove: Trajectory aborted (hit wall, hurt, or pit)", 128);
		return qfalse;
	}

	// Validate landing spot
	if (!NavMesh_IsPointOnMesh(pmove_land)) {
		if (failReason) Q_strncpyz(failReason, "Phantom Pmove: Landing spot is NOT on the NavMesh", 128);
		return qfalse;
	}

	if (out_land_pos) VectorCopy(pmove_land, out_land_pos);
	if (out_land_speed) *out_land_speed = max_run_speed;

	bot2_states[clientNum].tele_jumpSeq++;
	bot2_states[clientNum].tele_jumpStartTime = level.time;
	bot2_states[clientNum].tele_crossedZ = qfalse;
	bot2_states[clientNum].tele_predDir = testDir;
	bot2_states[clientNum].tele_takeoffYaw = vel_yaw;

	VectorCopy(ent->client->ps.origin, bot2_states[clientNum].tele_prevPos);
	VectorCopy(ent->client->ps.origin, bot2_states[clientNum].tele_startPos);

	bot2_states[clientNum].tele_takeoffSpd = current_speed;
	bot2_states[clientNum].tele_inAir = 1;

	// Populate telemetry with PMove's ground truth so the logs keep working
	VectorCopy(pmove_land, bot2_states[clientNum].tele_predPos);
	VectorCopy(pmove_land, bot2_states[clientNum].tele_pmovePredPos);

	float dx = pmove_land[0] - start[0], dy = pmove_land[1] - start[1];
	bot2_states[clientNum].tele_predDist = sqrt((dx * dx) + (dy * dy));

	return qtrue;
}

'''

new_content = content[:start_idx] + new_func + content[end_idx:]

with open('codemp/game/ai_bot2_nav.c', 'w') as f:
    f.write(new_content)

print('Success')