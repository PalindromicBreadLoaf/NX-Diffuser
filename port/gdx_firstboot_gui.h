// G-Diffuser — in-window first-time setup flow (ImGui).
//
// Replaces the old pre-window Win32-dialog wizard (see gdx_firstboot.{h,cpp}) with a setup screen
// drawn INSIDE the game window, in the SoH/BattleShip style: the window opens, this flow asks for the
// three original inputs (F-Zero X US rev0 ROM .z64, Expansion Kit disk .ndd, 64DD IPL ROM), validates
// and copies them beside the executable, runs the O2R extraction with live progress in the UI,
// hot-mounts the produced fzerox.o2r, and lets boot continue IN-PROCESS (no relaunch).
//
// It is invoked from main() only when FirstBootRun() returns FirstBootStatus::NeedsSetup, AFTER the
// window / Gui / FileDropMgr exist and BEFORE the game boots (between InitFileDropMgr and
// RegisterResourceFactories — everything after RegisterResourceFactories requires the ROM). It drives
// its own GUI-only frame pump (Window::RunGuiOnly) while no game threads run.
#pragma once

#include <string>

namespace gdx {

// Run the in-window setup flow. Blocks (pumping GUI-only frames) until the user completes setup or
// closes the window.
//
//   dataDir      Absolute data directory (== exeDir; where the inputs + fzerox.o2r live).
//   exeDir       Absolute executable directory (where gdx-extract + decomp-recipes ship).
//   outRomPath   On success, receives the absolute path of the installed ROM (for gdx_init_rom).
//
// Returns true when setup completed and boot should continue (the ROM/disk/IPL are installed, the
// completion marker is written, and — if extraction succeeded — fzerox.o2r has been hot-mounted).
// Returns false if the user closed the window during setup: the caller should exit cleanly. Any
// files already copied beside the exe persist, so the next launch resumes with those rows pre-filled.
bool GdxFirstBootSetupRun(const std::string& dataDir, const std::string& exeDir, std::string& outRomPath);

} // namespace gdx
