#pragma once
// Save import / export (F1 > Game > Save data; docs/NAND.md "Importing a Wii or Dolphin save"):
// a City Folk save from Dolphin's NAND, an extracted Wii NAND backup or the Wii's SD-card export
// (data.bin, with the player's own SD key) replaces the port's save at the next start, after a
// backup into <data folder>/save_backups/<timestamp>/; backups can be restored the same way; the
// port's save can be copied into a Dolphin user folder.
#include "recomp/ios.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace recomp::cf::saveimport {

// What applyAtStart changed in the NAND's attribute table (ios_fs.cpp records it).
struct AttrChange {
    std::string nandPath;
    bool forget = false;  // the file is gone
    ios::Attr attr;
};

// ios_fs.cpp nandRoot(), once, before anything reads the NAND (the game cannot be saving): applies a
// staged import or restore (and the CF_SAVE_IMPORT / CF_SAVE_RESTORE test aids). `attrOf` gives the
// recorded attributes of a NAND path (for the backup). Never calls back into the NAND layer.
std::vector<AttrChange> applyAtStart(const std::function<ios::Attr(const std::string&)>& attrOf);

// F1 > Game (menu.cpp): the "Save data" section.
void menuSection();
// Host, every frame (cf_game.cpp drawOverlay): CF_SAVE_EXPORT test aid.
void drawOverlay();

}  // namespace recomp::cf::saveimport
