# OP Click Buffer (Geode mod)

Keeps the game's button state equal to what you're holding whenever that wouldn't kill you, and when it would, simulates ahead with an invisible copy of your icon to find a timing that survives:

- early click → held until safe
- lethal click → dropped
- early release → held until letting go is safe
- missed click → if doing nothing dies within the rescue window, the latest surviving press/release/tap is injected for you

See `about.md` for details and limits.

Targets **Geometry Dash 2.2081** with **Geode 5.11.2** (Windows, Android, macOS, iOS).

## Getting the .geode file (no installs needed)

1. Make a new GitHub repo and upload everything in this folder (including the hidden `.github` folder).
2. Open the repo's **Actions** tab. The "Build Geode Mod" workflow runs automatically (or press *Run workflow*).
3. When it finishes (~5–10 min), open the run and download the **Build Output** artifact.
4. Unzip it and put `manpro.early-click-buffer.geode` into `Geometry Dash/geode/mods/`.

## Building locally (Windows)

Needs Visual Studio 2022 (C++ workload), CMake and the Geode CLI.

```
geode sdk install
geode sdk install-binaries
geode build
```

The .geode ends up in `build/`.

## Files

- `src/main.cpp` – the mod (Simulator = ghost physics, AssistPlayLayer = planner)
- `src/PlayerCopy.inc` – generated list of PlayerObject physics fields the ghost copies
- `tools/gen_player_copy.py` – regenerates it from the Geode bindings if GD updates
- `mod.json` – metadata and settings

## Known limits

- Moving objects (move/rotate triggers) are frozen in place during the prediction, so saves near moving hazards are less reliable.
- Prediction stops at gamemode/speed/mirror/dual/teleport portals and counts reaching them as surviving.
- Ship, swing, platformer and 2-player levels are passed through untouched.
- Don't run it together with Click Between Frames.
