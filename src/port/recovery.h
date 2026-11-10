#pragma once
// Start-up recovery and the run log (docs/SETUP.md "When the game does not start").
//
// Each run records how far it got in <configDir>/last_run_state.ini: starting (until the game has
// shown a few hundred frames), running, exited (clean) or crashed (with the error). When the last
// start crashed before the game ran, or two runs in a row failed, the next start offers recovery
// before anything else loads: reset the settings, rebuild the game data (recompiled module,
// shader caches) or put the town's save (the NAND title data folders) aside. Nothing is deleted that the
// player made: settings and saves are kept as backups.
//
// Windows: everything the game logs also goes to <configDir>/last_run.log (the previous run's to
// previous_run.log), since the console window closes with the game. Linux: the launcher script
// writes those files.
#include <cstddef>
#include <cstdio>

namespace port::recovery {

// After settings are loaded, before the graphics start. `canRebuild`: game data is built by
// setup (CityFolkRecomp), not linked in (cfrecomp). Shows the recovery prompt when needed and
// carries out the choice (the settings are reloaded if they were reset).
void onStart(bool canRebuild);
// The game has shown enough frames to count as started (frame.cpp).
void markRunning();
// Clean exit.
void onExit();
// The error text of a fatal error about to stop the game (Aurora FATAL, recomp fatal()).
void noteFatal(const char* message);
// From the crash handler: the run is crashing (no allocation; a short write).
void onCrash(const char* what);
// From the hang watchdog (crash_handler.cpp): while the run is still starting, the main thread has
// stopped making progress (`what`: how long and in which phase); recorded as the reason the recovery
// prompt shows if this start is then killed. nullptr: it continued (the reason is cleared).
void noteHang(const char* what);

// Windows, first thing in main: CityFolkRecomp is a windowed program (no console window for
// players). Output that was redirected (a pipe or file from the starting program) stays there;
// started from a terminal it prints to that terminal; CF_CONSOLE=1 or --console opens a console
// window (the old behaviour). Otherwise there is nowhere to print: stdout and stderr then go into
// last_run.log once openRunLog opens it, so nothing printed is lost.
void setupConsole(int argc, char** argv);
// Windows: opens last_run.log (moves the old one to previous_run.log). Null elsewhere / on failure.
void openRunLog();
std::FILE* runLog();
// Writes a line to the run log (if open), flushed. Nothing when stdout/stderr already go into the
// run log (setupConsole): the callers print the same text there.
void logLine(const char* text, std::size_t len);
// stdout and stderr are written into the run log.
bool stdioInLog();

}  // namespace port::recovery
