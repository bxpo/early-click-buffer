# Early Click Buffer (Geode mod)

Click a hair too early and your input waits a few physics ticks until it won't kill you.

On every click the mod simulates the next ticks with an invisible copy of your icon:
1. Simulate clicking **now**. If you live, the click goes straight through.
2. If you'd die, try clicking 1, 2, 3 … up to *Max Delay* ticks later.
3. The first delay that survives (and still actually jumps, if the on-time click would have) wins: your click is held exactly that long, then handed to the game.
4. If nothing within the window survives, the click goes through untouched.

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

- `src/main.cpp` – the mod
- `src/PlayerCopy.inc` – generated list of PlayerObject physics fields the ghost copies
- `tools/gen_player_copy.py` – regenerates it from the Geode bindings if GD updates
- `mod.json` – metadata and settings

## Known limits

- Moving objects (move/rotate triggers) are frozen in place during the prediction, so saves near moving hazards are less reliable.
- Prediction stops at gamemode/speed/mirror/dual/teleport portals and counts reaching them as surviving.
- Ship, swing, platformer and 2-player levels are passed through untouched.
- Don't run it together with Click Between Frames.
