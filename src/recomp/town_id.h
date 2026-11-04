#pragma once
// The loaded town's identity from the save (town id + name), one key for every feature that keeps
// something per town: Nintendo events' "Receive now" record (distributions.cpp, distrib_record.h)
// and the town WiiConnect24 default (online.cpp, <data>/town_wc24.txt). docs/DISTRIBUTIONS.md
// "Receive now once per town", docs/ONLINE.md section 10.
#include <string>

namespace recomp::town {

// The town data (USA 0x8074E960, the save's contents as fn_8010DD60 loads them): resident slots
// at +0x20 + slot * 0x86C0, each with a copy of the town struct at +0x7EC2 (u16 id, 9 UTF-16BE
// units); the first resident that exists. Key = distrib_record.h townKey ("<id>-<name hex>"),
// readable = "<name> (<id>)". "" while no town is loaded. Guest thread (reads guest memory).
std::string keyInSave(std::string* readable = nullptr);

}  // namespace recomp::town
