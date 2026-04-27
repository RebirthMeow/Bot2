# navmesh example files

*This document was written entirely by AI (Anthropic's Claude) based on automated codebase analysis, with maintainer review. Mistakes that slipped through review are possible — please file an issue if you spot one.*

This folder is the source-of-truth for `.navmesh` files distributed with Bot2.

## What goes here

- One or more `.navmesh` files — pre-compiled navmeshes for maps you want to ship as part of the Bot2 demo.
- Optionally, matching `.nav_connections` text sidecars (one per `.navmesh`) — Bot2's wallrun scanner output. Daemonmap-jka uses these on a Pass 2 build to bake wallrun connections; if you've baked them in already, the sidecar is redundant for runtime but harmless to ship.

The contents of this folder serve **two channels**:

1. **The git-tracked source**: anyone cloning the Bot2 repo can grab navmeshes from here directly.
2. **The Windows release zip**: `package-release.ps1` (at the repo root) defaults `-NavmeshDir` to this folder. Every `.navmesh` here gets copied into the release zip's `bot2_jka/maps/` payload automatically.

So curate this folder once; both distribution channels stay in sync.

## How to bake a `.navmesh` for a map

Use the companion tool: <https://github.com/RebirthMeow/daemonmap-jka>. Drag a JKA `.bsp` onto its `daemonmap-jka.bat` wrapper, copy the resulting `.navmesh` into this folder, commit.

For full wallrun support, follow the two-pass workflow described in daemonmap-jka's README — Pass 1 produces a basic navmesh, you run `/bot_scan_wallruns` in-game to generate a `.nav_connections` sidecar, Pass 2 bakes the sidecar into a wallrun-aware `.navmesh`. Drop the final `.navmesh` here.


