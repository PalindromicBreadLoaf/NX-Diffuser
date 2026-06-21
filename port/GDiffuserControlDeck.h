#pragma once
// G-Diffuser — controller deck (Slice 4c).
// libultraship's ControlDeck is abstract (WriteToPad is game-specific). Provide a concrete
// subclass so Context init succeeds. WriteToPad maps controller state into the game's pad
// format (N64 OSContPad) — stubbed for now; real input mapping comes once the game boots.

#include "ship/controller/controldeck/ControlDeck.h"

namespace GDiffuser {

class ControlDeck : public Ship::ControlDeck {
  public:
    // No extra buttons; default mappings (nullptr -> libultraship builds defaults); no custom names.
    ControlDeck() : Ship::ControlDeck({}, nullptr, {}) {}

    void WriteToPad(void* pads) override {
        (void)pads;
        // TODO (R6): translate each port's Controller state into F-Zero X's OSContPad buffer.
    }
};

} // namespace GDiffuser
