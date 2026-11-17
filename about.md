# Frame Counter

A tiny Geode mod for Geometry Dash that shows a plain-text counter in the top-left corner:

- **FPS** — live frames per second
- **BPS** — blocks per second while in a level
- **Frames** — total frames since launch

The counter turns **red** when your FPS drops below your threshold (default 55). Toggle each line, the threshold and the text size in Geode's mod settings — no restart needed.

It also includes an optional **240 FPS unlock** (target configurable), so the frame counter counts frame-perfect 1/240s frames — 1 frame at 240 fps, and 2 of those frames equal 1 frame of a 120 fps player.

And a built-in **macro recorder**: press **F5** in a level to record your inputs, **F6** to replay them on every attempt with a live `Macro: frame X / N` counter, **F7** to clear — or use the on-screen REC / PLAY / DEL menu with 4 macro slots per level. Macros are saved per level.

The counter is **draggable**: move it anywhere and it remembers. Replays are drift-free — inputs fire inside the game's own command loop, keyed to the level's internal clock — and with **Click Between Steps** they land at their exact sub-step within each tick, like showcase bots.
