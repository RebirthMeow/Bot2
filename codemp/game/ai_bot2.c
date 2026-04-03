#include "g_local.h"
#include "ai_main.h"
#include "ai_bot2.h"
#include "g_navmesh.h"

extern void G_Kill(gentity_t* ent);

// Bot2 State Machine Tracker
static struct {
	int state;           // 0 = Walk, 1 = Wind-up, 2 = Airborne
	int stateTimer;      // Tracks when the current state started
	float targetYaw;     // The original heading to the waypoint
	int strafeDir;       // 1 = Right, -1 = Left

	// --- TELEMETRY ENGINE ---
	int tele_inAir;          // 1 if we are currently tracking a jump
	float tele_takeoffSpd;   // Speed at the exact moment of prediction
	vec3_t tele_startPos;    // Where the jump started
	vec3_t tele_predPos;     // Where the bot THOUGHT it would land
	float tele_predDist;     // How far it THOUGHT it would travel horizontally
} bot2_states[MAX_CLIENTS] = { 0 };

/*
==============
Bot2_Think
==============
*/
void Bot2_Think(int clientNum, int time) {
	gentity_t* ent = &g_entities[clientNum];
	usercmd_t ucmd;
	char serverCmd[1024];

	// 1. UNCONDITIONAL MAILBOX DRAIN
	while (trap->BotGetServerCommand(ent->s.number, serverCmd, sizeof(serverCmd))) {
		// Read and discard.
	}

	// 2. UNCONDITIONAL HEARTBEAT
	ent->client->inactivityTime = level.time + 1000000;

	// 3. STATE BRANCHING
	if (ent->health <= 0) {
		memset(&ucmd, 0, sizeof(ucmd));
		ucmd.serverTime = time;
		ucmd.buttons = BUTTON_ATTACK;
		ucmd.angles[YAW] = ANGLE2SHORT(ent->client->ps.viewangles[YAW]) - ent->client->ps.delta_angles[YAW];
		ucmd.angles[PITCH] = ANGLE2SHORT(ent->client->ps.viewangles[PITCH]) - ent->client->ps.delta_angles[PITCH];

		// --- FATAL TELEMETRY LOG ---
		if (bot2_states[clientNum].tele_inAir && ent->client) {
			trap->Print(va("[Bot2 FATAL] Pit! Start {%.0f, %.0f, %.0f} | Pred {%.0f, %.0f, %.0f} | Dead {%.0f, %.0f, %.0f}\n",
				(double)bot2_states[clientNum].tele_startPos[0], (double)bot2_states[clientNum].tele_startPos[1], (double)bot2_states[clientNum].tele_startPos[2],
				(double)bot2_states[clientNum].tele_predPos[0], (double)bot2_states[clientNum].tele_predPos[1], (double)bot2_states[clientNum].tele_predPos[2],
				(double)ent->client->ps.origin[0], (double)ent->client->ps.origin[1], (double)ent->client->ps.origin[2]));
			bot2_states[clientNum].tele_inAir = 0; // Reset
		}

		// FAST RESPAWN: If we died and are falling fast (hit a pit trigger_hurt), 
		// bypass the 4-second falling death animation and respawn instantly.
		if (ent->client && (ent->client->ps.velocity[2] < -300.0f || ent->client->ps.fallingToDeath)) {
			ent->client->respawnTime = level.time;
			ent->client->ps.pm_time = 0;
		}
	}
	else {
		// PIT RECOVERY: If we've hit a "falling to death" trigger, kill us now 
		// so the dead branch can trigger the fast respawn.
		if (ent->client->ps.fallingToDeath) {
			G_Kill(ent);
		}

		ucmd = botstates[clientNum]->lastucmd;
		ucmd.serverTime = time;
		ucmd.buttons = 0;

		// 0. Physics Settle
		if (level.time - ent->client->pers.enterTime < 2000) {
			ucmd.forwardmove = 0;
			ucmd.rightmove = 0;
			ucmd.upmove = 0;
			ucmd.angles[YAW] = ANGLE2SHORT(ent->client->ps.viewangles[YAW]) - ent->client->ps.delta_angles[YAW];
			ucmd.angles[PITCH] = ANGLE2SHORT(ent->client->ps.viewangles[PITCH]) - ent->client->ps.delta_angles[PITCH];
		}
		else {
			vec3_t targetOrigin = { 0,0,0 };
			qboolean hasTarget = qfalse;
			vec3_t nextWp = { 0,0,0 };

			if (g_gametype.integer == GT_CTF) {
				int botTeam = ent->client->sess.sessionTeam;
				int enemyFlagItem = (botTeam == TEAM_RED) ? PW_BLUEFLAG : PW_REDFLAG;
				char* targetClass = (ent->client->ps.powerups[enemyFlagItem]) ?
					((botTeam == TEAM_RED) ? "team_CTF_redflag" : "team_CTF_blueflag") :
					((botTeam == TEAM_RED) ? "team_CTF_blueflag" : "team_CTF_redflag");

				gentity_t* flagEnt = G_Find(NULL, FOFS(classname), targetClass);
				if (flagEnt) {
					VectorCopy(flagEnt->s.origin, targetOrigin);
					hasTarget = qtrue;
				}
			}

			if (!hasTarget) {
				gentity_t* player = &g_entities[0];
				if (player && player->inuse && player->client && player != ent) {
					VectorCopy(player->client->ps.origin, targetOrigin);
					hasTarget = qtrue;
				}
			}

			if (hasTarget && VectorLength(targetOrigin) > 1.0f) {
				if (NavMesh_GetNextWaypoint(ent->s.number, (const float*)ent->client->ps.origin, (const float*)targetOrigin, (float*)nextWp)) {
					vec3_t dir, angles;
					VectorSubtract(nextWp, ent->client->ps.origin, dir);

					if (VectorLength(dir) > 0.1f) {
						vectoangles(dir, angles);
						float base_target_yaw = angles[YAW];

						int bState = bot2_states[clientNum].state;

						// --- STATE 0: WALK (Cooldown & Braking Phase) ---
						if (bState == 0) {
							float vel_x = ent->client->ps.velocity[0];
							float vel_y = ent->client->ps.velocity[1];
							float current_speed = sqrt((vel_x * vel_x) + (vel_y * vel_y));

							if (current_speed > 260.0f) {
								float vel_yaw = atan2(vel_y, vel_x) * (180.0f / M_PI);
								ucmd.angles[YAW] = ANGLE2SHORT(vel_yaw) - ent->client->ps.delta_angles[YAW];
								ucmd.angles[PITCH] = ANGLE2SHORT(0) - ent->client->ps.delta_angles[PITCH];

								ucmd.forwardmove = -127;
								ucmd.rightmove = 0;
								ucmd.upmove = 0;

								bot2_states[clientNum].stateTimer = level.time;
							}
							else {
								ucmd.angles[YAW] = ANGLE2SHORT(base_target_yaw) - ent->client->ps.delta_angles[YAW];
								ucmd.angles[PITCH] = ANGLE2SHORT(0) - ent->client->ps.delta_angles[PITCH];

								ucmd.forwardmove = 127;
								ucmd.rightmove = 0;
								ucmd.upmove = 0;

								if (level.time - bot2_states[clientNum].stateTimer > 2000) {
									bot2_states[clientNum].state = 1;
									bot2_states[clientNum].stateTimer = level.time;
									bot2_states[clientNum].targetYaw = base_target_yaw;

									vec3_t flatAngles = { 0, base_target_yaw, 0 };
									vec3_t forward, right;
									AngleVectors(flatAngles, forward, right, NULL);

									vec3_t start, forwardPos, endRight, endLeft;
									VectorCopy(ent->client->ps.origin, start);

									VectorMA(start, 64.0f, forward, forwardPos);
									VectorMA(forwardPos, 64.0f, right, endRight);
									VectorMA(forwardPos, -64.0f, right, endLeft);

									trace_t tr;
									vec3_t mins = { -15, -15, -24 };
									vec3_t maxs = { 15, 15, 32 };

									float scoreRight = 0.0f;
									float scoreLeft = 0.0f;

									trap->Trace(&tr, start, mins, maxs, endRight, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
									if (tr.fraction < 1.0f) scoreRight -= 10.0f;
									else {
										vec3_t downStart, downEnd;
										VectorCopy(tr.endpos, downStart);
										VectorCopy(downStart, downEnd);
										downEnd[2] -= 400.0f;
										trap->Trace(&tr, downStart, mins, maxs, downEnd, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
										if (tr.fraction == 1.0f || (tr.contents & (CONTENTS_LAVA | CONTENTS_SLIME))) scoreRight -= 1000.0f;
									}

									trap->Trace(&tr, start, mins, maxs, endLeft, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
									if (tr.fraction < 1.0f) scoreLeft -= 10.0f;
									else {
										vec3_t downStart, downEnd;
										VectorCopy(tr.endpos, downStart);
										VectorCopy(downStart, downEnd);
										downEnd[2] -= 400.0f;
										trap->Trace(&tr, downStart, mins, maxs, downEnd, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
										if (tr.fraction == 1.0f || (tr.contents & (CONTENTS_LAVA | CONTENTS_SLIME))) scoreLeft -= 1000.0f;
									}

									if (scoreLeft > scoreRight) { bot2_states[clientNum].strafeDir = -1; }
									else if (scoreRight > scoreLeft) { bot2_states[clientNum].strafeDir = 1; }
									else { bot2_states[clientNum].strafeDir = (rand() % 2 == 0) ? 1 : -1; }
								}
							}
						}
						// --- STATE 1: WIND-UP ---
						else if (bState == 1) {
							int sDir = bot2_states[clientNum].strafeDir;

							ucmd.forwardmove = 127;
							ucmd.rightmove = 127 * sDir;
							ucmd.upmove = 0;

							float windup_yaw = bot2_states[clientNum].targetYaw + (60.0f * sDir);
							windup_yaw = AngleMod(windup_yaw);

							ucmd.angles[YAW] = ANGLE2SHORT(windup_yaw) - ent->client->ps.delta_angles[YAW];
							ucmd.angles[PITCH] = ANGLE2SHORT(0) - ent->client->ps.delta_angles[PITCH];

							if (level.time - bot2_states[clientNum].stateTimer > 200) {
								float vx = ent->client->ps.velocity[0];
								float vy = ent->client->ps.velocity[1];
								float current_speed = sqrt((vx * vx) + (vy * vy));

								qboolean safeToJump = qtrue;

								if (current_speed > 50.0f) {
									trace_t tr;
									vec3_t start, test_pos;

									float sweep_width = current_speed * 0.1f;
									if (sweep_width > 45.0f) sweep_width = 45.0f;
									vec3_t sweep_mins = { -15 - sweep_width, -15 - sweep_width, -24 };
									vec3_t sweep_maxs = { 15 + sweep_width, 15 + sweep_width, 32 };

									float max_speed = (ent->client->ps.speed > 0.0f) ? ent->client->ps.speed : 250.0f;
									float magic_angle = 0.0f;
									if (current_speed > max_speed - 15.0f) {
										float acos_val = (max_speed - 15.0f) / current_speed;
										if (acos_val < -1.0f) acos_val = -1.0f;
										if (acos_val > 1.0f) acos_val = 1.0f;
										magic_angle = acos(acos_val) * (180.0f / M_PI);
									}

									float dynamic_mult = 1.6f - (current_speed / 2000.0f);
									if (dynamic_mult < 1.0f) dynamic_mult = 1.0f;
									float avg_speed = current_speed * dynamic_mult;
									if (avg_speed < 250.0f) avg_speed = 250.0f;

									float base_dist = (avg_speed * 0.675f) + 40.0f;

									float vel_yaw = atan2(vy, vx) * (180.0f / M_PI);
									float curve_yaw = vel_yaw + (((magic_angle - 45.0f) * sDir) * 0.5f);
									float rad = curve_yaw * (M_PI / 180.0f);

									float trace_dx = cos(rad);
									float trace_dy = sin(rad);

									VectorCopy(ent->client->ps.origin, start);

									vec3_t mid_pos, midDownStart, midDownEnd;
									mid_pos[0] = start[0] + trace_dx * (base_dist * 0.5f);
									mid_pos[1] = start[1] + trace_dy * (base_dist * 0.5f);
									mid_pos[2] = start[2] + 32.0f;

									trap->Trace(&tr, start, sweep_mins, sweep_maxs, mid_pos, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
									if (tr.fraction < 0.2f) {
										safeToJump = qfalse;
									}
									else {
										VectorCopy(tr.endpos, midDownStart);
										VectorCopy(midDownStart, midDownEnd);
										midDownEnd[2] -= 2048.0f;
										trap->Trace(&tr, midDownStart, sweep_mins, sweep_maxs, midDownEnd, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

										if (tr.fraction == 1.0f || (tr.contents & (CONTENTS_LAVA | CONTENTS_SLIME)) || (start[2] - tr.endpos[2] > 400.0f)) {
											safeToJump = qfalse;
										}
									}

									if (safeToJump) {
										test_pos[0] = start[0] + trace_dx * base_dist;
										test_pos[1] = start[1] + trace_dy * base_dist;
										test_pos[2] = start[2] + 32.0f;

										trap->Trace(&tr, mid_pos, sweep_mins, sweep_maxs, test_pos, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

										vec3_t downStart, downEnd;
										VectorCopy(tr.endpos, downStart);
										VectorCopy(downStart, downEnd);
										downEnd[2] -= 2048.0f;
										trap->Trace(&tr, downStart, sweep_mins, sweep_maxs, downEnd, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

										if (tr.fraction == 1.0f || (tr.contents & (CONTENTS_LAVA | CONTENTS_SLIME))) safeToJump = qfalse;
										else {
											float dropHeight = start[2] - tr.endpos[2];
											float touchdown_dist = base_dist;
											float effective_drop = 0.0f; // Track this for the reality check

											if (dropHeight > 16.0f || dropHeight < -16.0f) {
												effective_drop = dropHeight;
												if (effective_drop < -45.0f) effective_drop = -45.0f;
												if (effective_drop > 800.0f) effective_drop = 800.0f;

												float fall_time = sqrt((45.5f + effective_drop) / 400.0f);
												touchdown_dist = (avg_speed * (0.3375f + fall_time)) + 40.0f;
											}

											float skid_buffer = current_speed * 0.50f;
											if (skid_buffer < 64.0f) skid_buffer = 64.0f;
											float final_slide_dist = touchdown_dist + skid_buffer;

											test_pos[0] = start[0] + trace_dx * final_slide_dist;
											test_pos[1] = start[1] + trace_dy * final_slide_dist;
											test_pos[2] = start[2] + 32.0f;

											trap->Trace(&tr, start, sweep_mins, sweep_maxs, test_pos, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

											if (tr.fraction < 0.2f) {
												safeToJump = qfalse;
											}
											else {
												VectorCopy(tr.endpos, downStart);
												VectorCopy(downStart, downEnd);
												downEnd[2] -= 2048.0f;
												trap->Trace(&tr, downStart, sweep_mins, sweep_maxs, downEnd, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

												float finalDropHeight = start[2] - tr.endpos[2];

												if (tr.fraction == 1.0f || (tr.contents & (CONTENTS_LAVA | CONTENTS_SLIME)) || finalDropHeight > 400.0f) {
													safeToJump = qfalse;
												}
												// FIX: THE GAP-SPANNING ILLUSION CHECK
												else if (effective_drop > 64.0f && finalDropHeight < (effective_drop - 128.0f)) {
													safeToJump = qfalse;
												}
												else {
													safeToJump = qtrue;
													VectorCopy(ent->client->ps.origin, bot2_states[clientNum].tele_startPos);
													VectorCopy(tr.endpos, bot2_states[clientNum].tele_predPos);
													bot2_states[clientNum].tele_predDist = touchdown_dist;
													bot2_states[clientNum].tele_takeoffSpd = current_speed;
													bot2_states[clientNum].tele_inAir = 1;
												}
											}
										}
									}
								}

								if (safeToJump) {
									ucmd.upmove = 127;
									bot2_states[clientNum].state = 2;
									bot2_states[clientNum].stateTimer = level.time;
								}
								else {
									bot2_states[clientNum].state = 0;
									bot2_states[clientNum].stateTimer = level.time;
								}
							}
						}
						// --- STATE 2: AIRBORNE ---
						else if (bState == 2) {
							int sDir = bot2_states[clientNum].strafeDir;

							float vel_x = ent->client->ps.velocity[0];
							float vel_y = ent->client->ps.velocity[1];
							float current_speed = sqrt((vel_x * vel_x) + (vel_y * vel_y));
							float vel_yaw = atan2(vel_y, vel_x) * (180.0f / M_PI);

							float max_speed = (ent->client->ps.speed > 0.0f) ? ent->client->ps.speed : 250.0f;
							float magic_angle = 0.0f;

							if (current_speed > max_speed - 15.0f) {
								float acos_val = (max_speed - 15.0f) / current_speed;
								if (acos_val < -1.0f) acos_val = -1.0f;
								if (acos_val > 1.0f) acos_val = 1.0f;
								magic_angle = acos(acos_val) * (180.0f / M_PI);
							}

							ucmd.forwardmove = 127;
							ucmd.rightmove = 127 * sDir;

							float flight_yaw = vel_yaw - ((magic_angle - 45.0f) * sDir);
							flight_yaw = AngleMod(flight_yaw);

							ucmd.angles[YAW] = ANGLE2SHORT(flight_yaw) - ent->client->ps.delta_angles[YAW];
							ucmd.angles[PITCH] = ANGLE2SHORT(0) - ent->client->ps.delta_angles[PITCH];

							if (level.time - bot2_states[clientNum].stateTimer < 100) ucmd.upmove = 127;
							else ucmd.upmove = 0;

							// --- TOUCHDOWN LOGIC ---
							if (ent->client->ps.groundEntityNum != ENTITYNUM_NONE && (level.time - bot2_states[clientNum].stateTimer > 250)) {

								// --- EVALUATE TELEMETRY ON LANDING ---
								if (bot2_states[clientNum].tele_inAir && ent->client) {
									float dx = ent->client->ps.origin[0] - bot2_states[clientNum].tele_startPos[0];
									float dy = ent->client->ps.origin[1] - bot2_states[clientNum].tele_startPos[1];
									float actualDist = sqrt((dx * dx) + (dy * dy));
									float error = actualDist - bot2_states[clientNum].tele_predDist;

									trap->Print(va("[Bot2] Err: %+.1f | TakeoffSpd: %.0f | LandSpd: %.0f | Yaw: %.0f\n  -> Start: {%.0f, %.0f, %.0f}\n  -> Pred:  {%.0f, %.0f, %.0f} (Dist: %.1f)\n  -> Act:   {%.0f, %.0f, %.0f} (Dist: %.1f)\n",
										(double)error, (double)bot2_states[clientNum].tele_takeoffSpd, (double)current_speed, (double)vel_yaw,
										(double)bot2_states[clientNum].tele_startPos[0], (double)bot2_states[clientNum].tele_startPos[1], (double)bot2_states[clientNum].tele_startPos[2],
										(double)bot2_states[clientNum].tele_predPos[0], (double)bot2_states[clientNum].tele_predPos[1], (double)bot2_states[clientNum].tele_predPos[2], (double)bot2_states[clientNum].tele_predDist,
										(double)ent->client->ps.origin[0], (double)ent->client->ps.origin[1], (double)ent->client->ps.origin[2], (double)actualDist));

									bot2_states[clientNum].tele_inAir = 0;
								}

								float yaw_diff = vel_yaw - base_target_yaw;
								while (yaw_diff > 180.0f) yaw_diff -= 360.0f;
								while (yaw_diff < -180.0f) yaw_diff += 360.0f;
								if (yaw_diff < 0) yaw_diff = -yaw_diff;

								qboolean safeToChainJump = qfalse;
								int nextDirToUse = sDir * -1;

								float max_safe_yaw_diff = 15000.0f / (current_speed > 100.0f ? current_speed : 100.0f);
								if (max_safe_yaw_diff > 60.0f) max_safe_yaw_diff = 60.0f;

								if (current_speed > 280.0f && yaw_diff > max_safe_yaw_diff) {
									safeToChainJump = qfalse;
								}
								else if (current_speed > 50.0f) {
									int testDirs[2] = { sDir * -1, sDir };
									int d;
									for (d = 0; d < 2; d++) {
										trace_t tr;
										vec3_t start, test_pos;

										float sweep_width = current_speed * 0.1f;
										if (sweep_width > 45.0f) sweep_width = 45.0f;
										vec3_t sweep_mins = { -15 - sweep_width, -15 - sweep_width, -24 };
										vec3_t sweep_maxs = { 15 + sweep_width, 15 + sweep_width, 32 };

										float dynamic_mult = 1.6f - (current_speed / 2000.0f);
										if (dynamic_mult < 1.0f) dynamic_mult = 1.0f;
										float avg_speed = current_speed * dynamic_mult;
										if (avg_speed < 250.0f) avg_speed = 250.0f;

										float base_dist = (avg_speed * 0.675f) + 40.0f;

										// FIX: Anchor chained predictions to the NavMesh path, not the landing momentum.
										float curve_yaw = base_target_yaw + (((magic_angle - 45.0f) * testDirs[d]) * 0.5f);
										float rad = curve_yaw * (M_PI / 180.0f);

										float trace_dx = cos(rad);
										float trace_dy = sin(rad);

										VectorCopy(ent->client->ps.origin, start);

										vec3_t mid_pos, midDownStart, midDownEnd;
										mid_pos[0] = start[0] + trace_dx * (base_dist * 0.5f);
										mid_pos[1] = start[1] + trace_dy * (base_dist * 0.5f);
										mid_pos[2] = start[2] + 32.0f;

										trap->Trace(&tr, start, sweep_mins, sweep_maxs, mid_pos, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);
										if (tr.fraction < 0.2f) {
											safeToChainJump = qfalse;
										}
										else {
											VectorCopy(tr.endpos, midDownStart);
											VectorCopy(midDownStart, midDownEnd);
											midDownEnd[2] -= 2048.0f;
											trap->Trace(&tr, midDownStart, sweep_mins, sweep_maxs, midDownEnd, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

											if (tr.fraction == 1.0f || (tr.contents & (CONTENTS_LAVA | CONTENTS_SLIME)) || (start[2] - tr.endpos[2] > 400.0f)) {
												safeToChainJump = qfalse;
											}
											else {
												safeToChainJump = qtrue;
											}
										}

										if (safeToChainJump) {
											test_pos[0] = start[0] + trace_dx * base_dist;
											test_pos[1] = start[1] + trace_dy * base_dist;
											test_pos[2] = start[2] + 32.0f;

											trap->Trace(&tr, mid_pos, sweep_mins, sweep_maxs, test_pos, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

											vec3_t downStart, downEnd;
											VectorCopy(tr.endpos, downStart);
											VectorCopy(downStart, downEnd);
											downEnd[2] -= 2048.0f;
											trap->Trace(&tr, downStart, sweep_mins, sweep_maxs, downEnd, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

											if (tr.fraction == 1.0f || (tr.contents & (CONTENTS_LAVA | CONTENTS_SLIME))) {
												safeToChainJump = qfalse;
											}
											else {
												float dropHeight = start[2] - tr.endpos[2];
												float touchdown_dist = base_dist;
												float effective_drop = 0.0f; // Track this for the reality check

												if (dropHeight > 16.0f || dropHeight < -16.0f) {
													effective_drop = dropHeight;
													if (effective_drop < -45.0f) effective_drop = -45.0f;
													if (effective_drop > 800.0f) effective_drop = 800.0f;

													float fall_time = sqrt((45.5f + effective_drop) / 400.0f);
													touchdown_dist = (avg_speed * (0.3375f + fall_time)) + 40.0f;
												}

												float skid_buffer = current_speed * 0.50f;
												if (skid_buffer < 64.0f) skid_buffer = 64.0f;
												float final_slide_dist = touchdown_dist + skid_buffer;

												test_pos[0] = start[0] + trace_dx * final_slide_dist;
												test_pos[1] = start[1] + trace_dy * final_slide_dist;
												test_pos[2] = start[2] + 32.0f;

												trap->Trace(&tr, start, sweep_mins, sweep_maxs, test_pos, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

												if (tr.fraction < 0.2f) {
													safeToChainJump = qfalse;
												}
												else {
													VectorCopy(tr.endpos, downStart);
													VectorCopy(downStart, downEnd);
													downEnd[2] -= 2048.0f;
													trap->Trace(&tr, downStart, sweep_mins, sweep_maxs, downEnd, ent->s.number, MASK_PLAYERSOLID, qfalse, 0, 0);

													float finalDropHeight = start[2] - tr.endpos[2];

													if (tr.fraction == 1.0f || (tr.contents & (CONTENTS_LAVA | CONTENTS_SLIME)) || finalDropHeight > 400.0f) {
														safeToChainJump = qfalse;
													}
													// FIX: THE GAP-SPANNING ILLUSION CHECK
													else if (effective_drop > 64.0f && finalDropHeight < (effective_drop - 128.0f)) {
														safeToChainJump = qfalse;
													}
													else {
														safeToChainJump = qtrue;
														nextDirToUse = testDirs[d];

														VectorCopy(ent->client->ps.origin, bot2_states[clientNum].tele_startPos);
														VectorCopy(tr.endpos, bot2_states[clientNum].tele_predPos);
														bot2_states[clientNum].tele_predDist = touchdown_dist;
														bot2_states[clientNum].tele_takeoffSpd = current_speed;
														bot2_states[clientNum].tele_inAir = 1;

														break;
													}
												}
											}
										}
									}
								}
								else safeToChainJump = qtrue;

								if (safeToChainJump) {
									bot2_states[clientNum].strafeDir = nextDirToUse;
									ucmd.upmove = 127;
									bot2_states[clientNum].stateTimer = level.time;
								}
								else {
									bot2_states[clientNum].state = 0;
									bot2_states[clientNum].stateTimer = level.time;
								}
							}
						}

						ucmd.angles[ROLL] = ANGLE2SHORT(angles[ROLL]) - ent->client->ps.delta_angles[ROLL];

					}
					else {
						ucmd.forwardmove = 0;
						ucmd.rightmove = 0;
						ucmd.upmove = 0;
						bot2_states[clientNum].state = 0;
					}
				}
				else {
					ucmd.forwardmove = 0;
					ucmd.rightmove = 0;
					ucmd.upmove = 0;
					bot2_states[clientNum].state = 0;
				}
			}
			else {
				ucmd.forwardmove = 0;
				ucmd.rightmove = 0;
				ucmd.upmove = 0;
				bot2_states[clientNum].state = 0;
			}
		}
	}

	botstates[clientNum]->lastucmd = ucmd;
	trap->BotUserCommand(ent->s.number, &ucmd);
}
