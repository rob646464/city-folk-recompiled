// Nintendo events: the per-town record (distrib_record.h; docs/DISTRIBUTIONS.md "The record").
#include "recomp/distrib_record.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <istream>
#include <ostream>

namespace recomp::distrib::record {

namespace {
std::string hex32(u32 v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08X", v);
    return b;
}
}  // namespace

void parse(Book& b, std::istream& in) {
    std::string line, section;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') {
            section = line.substr(1, line.find(']') - 1);
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (section == "state") {
            if (k == "town") b.town = v;
            else if (k == "format") b.format = std::atoi(v.c_str());
            else if (k == "manual_counter") b.manualCounter = u32(std::strtoul(v.c_str(), nullptr, 10));
            else if (k == "last_stored") b.lastStored = u32(std::strtoul(v.c_str(), nullptr, 16));
        } else if (section.rfind("town.", 0) == 0) {
            TownState& t = b.towns[section.substr(5)];
            if (k == "last_delivery") t.lastDelivery = std::atoll(v.c_str());
            else {
                long long when = 0;
                int cyc = 0;
                std::sscanf(v.c_str(), "%lld,%d", &when, &cyc);
                t.received[k] = when;
                t.cycle[k] = cyc;
            }
        } else if (section.rfind("received_now.", 0) == 0) {
            b.towns[section.substr(13)].now[k] = std::atoll(v.c_str());
        } else if (section == "towns") {
            b.names[k] = v;
        } else if (section == "served") {
            b.served[u32(std::strtoul(k.c_str(), nullptr, 16))] = std::atoi(v.c_str());
        }
    }
}

void write(const Book& b, std::ostream& f) {
    f << "# Nintendo events: what each town received (written by the game; docs/DISTRIBUTIONS.md)\n";
    f << "# Towns are keyed by the town id + name from the save. [received_now.*] = distributions the\n"
         "# town got through \"Receive now\" (once per town); the schedules never change it.\n";
    f << "[state]\nformat=" << b.format << "\ntown=" << b.town << "\nmanual_counter=" << b.manualCounter
      << "\nlast_stored=" << hex32(b.lastStored) << "\n";
    if (!b.names.empty()) {
        f << "[towns]\n";
        for (const auto& [k, n] : b.names) f << k << "=" << n << "\n";
    }
    for (const auto& [name, t] : b.towns) {
        f << "[town." << name << "]\nlast_delivery=" << t.lastDelivery << "\n";
        for (const auto& [k, when] : t.received) {
            const auto c = t.cycle.find(k);
            f << k << "=" << when << "," << (c == t.cycle.end() ? 0 : c->second) << "\n";
        }
        if (!t.now.empty()) {
            f << "[received_now." << name << "]\n";
            for (const auto& [k, when] : t.now) f << k << "=" << when << "\n";
        }
    }
    f << "[served]\n";
    for (const auto& [id, d] : b.served) f << hex32(id) << "=" << d << "\n";
}

bool upgrade(Book& b, const std::function<const char*(int)>& keyOf) {
    if (b.format >= kFormat) return false;
    for (auto& [townName, t] : b.towns)
        for (const auto& [id, di] : b.served) {
            if (!isReceiveNowId(id)) continue;
            const char* key = keyOf(di);
            if (!key) continue;
            const auto r = t.received.find(key);
            if (r != t.received.end() && !t.now.count(key)) t.now[key] = r->second;
        }
    b.format = kFormat;
    return true;
}

std::string townKey(const u8* s, std::string* readable) {
    const unsigned id = unsigned(s[0]) << 8 | s[1];
    if (id == 0 || id == 0xFFFF) return "";
    std::string key, utf8;
    char b[8];
    std::snprintf(b, sizeof b, "%04X-", id);
    key = b;
    int n = 0;
    for (int i = 0; i < 9; i++) {
        const unsigned ch = unsigned(s[2 + 2 * i]) << 8 | s[3 + 2 * i];
        if (!ch) break;
        std::snprintf(b, sizeof b, "%04X", ch);
        key += b;
        n++;
        if (ch < 0x80) utf8 += char(ch);
        else if (ch < 0x800) utf8 += char(0xC0 | ch >> 6), utf8 += char(0x80 | (ch & 0x3F));
        else utf8 += char(0xE0 | ch >> 12), utf8 += char(0x80 | (ch >> 6 & 0x3F)), utf8 += char(0x80 | (ch & 0x3F));
    }
    if (!n) return "";
    if (readable) {
        std::snprintf(b, sizeof b, "%04X", id);
        *readable = utf8 + " (" + b + ")";
    }
    return key;
}

bool identify(Book& b, const std::string& key, const std::string& readable) {
    if (key.empty()) return false;
    bool changed = false;
    if (!readable.empty() && b.names[key] != readable) b.names[key] = readable, changed = true;
    if (b.town == key) return changed;
    if (b.town == kLegacyTown) {
        const auto old = b.towns.find(kLegacyTown);
        if (old != b.towns.end()) {
            TownState& t = b.towns[key];
            const TownState& o = old->second;
            for (const auto& [k, when] : o.received)
                if (!t.received.count(k) || t.received[k] < when) {
                    t.received[k] = when;
                    const auto c = o.cycle.find(k);
                    t.cycle[k] = c == o.cycle.end() ? 0 : c->second;
                }
            for (const auto& [k, when] : o.now)
                if (!t.now.count(k)) t.now[k] = when;
            t.lastDelivery = std::max(t.lastDelivery, o.lastDelivery);
            b.towns.erase(old);
        }
    }
    b.town = key;
    return true;
}

}  // namespace recomp::distrib::record
