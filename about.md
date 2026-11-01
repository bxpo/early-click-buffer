# OP Click Buffer

Every tick the mod keeps two things in mind: what you're **holding** and what the **game** has. It keeps them equal whenever that wouldn't kill you. When it would, it runs the game's own physics on an invisible copy of your icon and finds a timing that survives.

## What it fixes
- **Early click** – held back until the tick it stops being lethal.
- **Wrong click** – if no delay makes it safe, it never reaches the game.
- **Early release** – the button stays held (orb chains, wave) until letting go is safe.
- **Missed click** – if doing nothing would kill you within the rescue window, the mod presses, releases or taps for you at the latest moment that still survives.

Works in every gamemode and in normal duals. Platformer and 2-player levels are left alone.

## Settings worth knowing
- **Rescue Window** – how close a death has to be before the mod takes over. 32 ticks (≈0.13 s) means you stay in control until roughly human-reaction time before dying.
- **Max Input Delay** – how long your own clicks are retried every tick before backing off.
- **Auto-Rescue** – turn this off to go back to just click buffering.

## Limits
- Moving objects are treated as standing still during prediction, so moving hazards can still get you.
- Prediction stops at gamemode/speed/mirror/dual/teleport portals and counts reaching them as surviving.
- Only one change of direction is searched per rescue, so a section that needs a precise *sequence* of inputs can still kill you if you give it nothing.

This changes your inputs, so runs with it won't count for leaderboards or list verification. Don't run it with Click Between Frames.
