# port/ — Game-specific glue layer

Code that belongs to G-Diffuser specifically (kept out of `libultraship/` so the engine submodule
stays clean and upstream-mergeable). Mirrors the role of BattleShip's `port/` directory.

Will contain (lands incrementally from M1):

- **Context / entry point** — application bootstrap on top of libultraship (`Ship::Context`).
- **Resource factories** — register F-Zero X resource types with the O2R resource system.
- **OS / libultra shims** — replace N64 `libultra` calls the decomp makes (threads, messaging, PI/SI,
  controller, RDP/RSP task submission) with libultraship equivalents.
- **Main loop glue** — wire the decomp's game loop to libultraship's frame/audio/input pumps.

Nothing here yet — this is the M0 scaffold.
