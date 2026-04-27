// ==============================================================================
// ai_bot2_nav.c - DEPRECATED
//
// The contents of this file were split into:
//
//   ai_bot2_pmove.c    - Pmove simulator, jump-chain evaluator, IsSafeToJump,
//                        Bot2_PMTrace/Bot2_PMPointContents/BotBreadcrumb.
//   ai_bot2_combat.c   - Bot2_GetLeadOrigin (projectile lead computation),
//                        Bot2_ApplySmoothing (clamped angle blending).
//   ai_bot2_wallrun.c  - Wallrun simulators (Scenario / ScenarioEx / Ex / regular),
//                        map scanners (Bot2_ScanWallruns / *Headless),
//                        Bot2_WallrunCheck developer command.
//   ai_bot2_movement.c - Floor/elevator helpers, Bot2_FindEscapeJump,
//                        Bot2_ExecuteMovement (8-state movement driver).
//
// Cross-file private helpers are declared in ai_bot2_internal.h.
//
// This file is intentionally empty — remove it from CMakeLists.txt and delete
// it from disk at your convenience.
// ==============================================================================
