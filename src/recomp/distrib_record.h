#pragma once
// Nintendo events: the per-town record in <data folder>/distributions/state.ini
// (docs/DISTRIBUTIONS.md "The record"). Self-contained (no guest memory, no settings) so the unit
// test (tools/tests/distrib_test.cpp) can check parsing, the upgrade of older files and the
// identification of a town.
//
// A town is identified by its own data from the save: the town id (u16, made when the town was
// created) and the town name, as every resident's player data carries them (pd + 0x7EC2: u16 id,
// 9 UTF-16BE units; docs/FEATURES.md). Key = "<id, 4 hex>-<name, UTF-16BE hex>", so a copied or
// renamed data folder keeps its record, and another town has its own.
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <map>
#include <string>

namespace recomp::distrib::record {

using u8 = std::uint8_t;
using u32 = std::uint32_t;

// The town key every file before the town-identity change used (one record per install).
constexpr const char* kLegacyTown = "town";

struct TownState {
    std::map<std::string, long long> received;  // key -> console time (s since 2000) the game took it (any route)
    std::map<std::string, int> cycle;           // key -> loop cycle it was received in
    long long lastDelivery = 0;
    // "Receive now": key -> console time the game kept it. At most once per town; the schedules
    // never write or clear this. Original .enc files from the folder: "enc:<SHA-1 of the file>".
    std::map<std::string, long long> now;
};

struct Book {
    int format = 0;                      // 0/1 = before the "Receive now once" change, 2 = now
    std::string town = kLegacyTown;      // the current (last identified) town's key
    std::map<std::string, TownState> towns;
    std::map<std::string, std::string> names;  // town key -> readable name ("Frunch (F177)")
    u32 manualCounter = 0;
    u32 lastStored = 0;           // letter id of the package KD stored last
    std::map<u32, int> served;    // letter id -> catalogue index (what each id carried)
};

constexpr int kFormat = 2;
// Letter ids of "Receive now" packages: 0x4D ('M') + counter.
constexpr bool isReceiveNowId(u32 id) { return (id >> 24) == 0x4D; }

void parse(Book& b, std::istream& in);
void write(const Book& b, std::ostream& out);

// Files from before format 2: a distribution counts as received through "Receive now" when the
// town's record has it and a "Receive now" letter id (0x4D...) was served for it. keyOf maps a
// catalogue index to its key (nullptr: unknown index). Sets format = 2. True when anything changed.
bool upgrade(Book& b, const std::function<const char*(int)>& keyOf);

// The town struct (0x14 bytes at pd + 0x7EC2: u16 id BE, up to 9 UTF-16BE units) -> key, or ""
// when it names no town (id 0 / 0xFFFF, empty name). readable gets "<name> (<id>)" in UTF-8.
std::string townKey(const u8* townStruct, std::string* readable = nullptr);

// The game shows town `key`: it becomes the current town. The legacy record ("town") of a file
// from before the change belongs to the first town identified on that install (City Folk has one
// town per console), so it is merged into that town's record once. True when anything changed.
bool identify(Book& b, const std::string& key, const std::string& readable);

}  // namespace recomp::distrib::record
