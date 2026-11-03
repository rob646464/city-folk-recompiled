// Settings while visiting (online_sync.h, docs/ONLINE.md "Settings while visiting").
//
// Blob: ini text as settings.ini has it, first line "cfsync <version>". The host sends its synced
// keys plus its [skin_tone] section to a visitor right before accepting it into the match
// (online_dwc.cpp onJoinRequest); the visitor applies the synced keys as a settings override
// (port::beginSettingsOverride) and the tones as other players' tones (cf::skinSetRemote), and
// sends its own [skin_tone] back. When the match ends (back home, a dropped connection, the room
// gone, DWC shut down) the override ends and the visitor's own values return; settings.ini only
// ever gets the visitor's own values.
#include "recomp/online_sync.h"

#include "game/cf_features.h"
#include "port/settings.h"
#include "recomp/online.h"

#include <imgui.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

namespace recomp::online::sync {
namespace {

std::mutex sMutex;
bool sVisiting = false;
std::string sHostName;
std::map<uint8_t, std::map<std::string, int>> sTones;  // member -> its tones

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(i);
}

// Lines of ini text with their section.
template <typename F>
void eachLine(const std::string& text, F f) {
    std::istringstream in(text);
    std::string line, section;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[') {
            section = line.substr(1, line.find(']') - 1);
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        f(section, line, trim(line.substr(0, eq)), trim(line.substr(eq + 1)));
    }
}

void pushTones() {
    std::map<std::string, int> all;
    for (const auto& [m, t] : sTones)
        for (const auto& [k, v] : t) all[k] = v;
    cf::skinSetRemote(all);
}

std::string blob(bool settingsToo) {
    const std::string text = port::settingsText();
    std::ostringstream out;
    out << "cfsync " << kVersion << "\n";
    std::string last;
    eachLine(text, [&](const std::string& section, const std::string& line, const std::string& key, const std::string&) {
        if (section == "skin_tone" || (settingsToo && synced(section, key))) {
            if (section != last) {
                out << "[" << section << "]\n";
                last = section;
            }
            out << line << "\n";
        }
    });
    return out.str();
}

bool logOn() { return online::logOn(); }

}  // namespace

bool synced(const std::string& section, const std::string& rawKey) {
    const std::string key = trim(rawKey);
    // The shared world: what the town shows and how it behaves for everyone in it.
    if (section == "calendar") return true;                          // hemisphere (seasons, events)
    if (section == "region") return key != "title_screen";           // region events/content, villager houses
    if (section == "weather") return true;                           // real-time weather: the host's place
    if (section == "gameplay") return key == "grass_wear";           // grass wear of the town's ground
    if (section == "nintendo_events") return key == "mode" || key == "loop_period" || key == "original_yearly" || key == "other_regions";
    return false;  // presentation, audio, controls, interface, own skin tone, online, system (clock, language)
}

std::string hostBlob() { return blob(true); }
std::string toneBlob() { return blob(false); }

void onBlob(uint8_t member, const std::string& text, bool fromServer, const std::string& senderName) {
    if (text.size() > 64 * 1024) return;
    std::istringstream head(text);
    std::string magic;
    int version = 0;
    head >> magic >> version;
    if (magic != "cfsync" || version < 1) {
        std::printf("[sync] settings from member %u ignored (format %s %d)\n", member, magic.c_str(), version);
        return;
    }
    std::lock_guard<std::mutex> g(sMutex);
    // Tones: only "<4 hex>-<4 hex>" keys, 1..8.
    std::map<std::string, int> tones;
    eachLine(text, [&](const std::string& section, const std::string&, const std::string& key, const std::string& value) {
        if (section != "skin_tone" || key.size() != 9 || key[4] != '-') return;
        const int t = std::atoi(value.c_str());
        if (t >= 1 && t <= 8) tones[key] = t;
    });
    sTones[member] = tones;
    pushTones();
    if (fromServer) {
        // Newer formats may add keys; only keys this version syncs are applied.
        port::beginSettingsOverride(text, [](const std::string& s, const std::string& k) { return synced(s, k); });
        sVisiting = true;
        sHostName = senderName.empty() ? "the host" : senderName;
        std::printf("[sync] visiting %s: using the host's world settings (%zu tones)\n", sHostName.c_str(), tones.size());
        if (logOn())
            eachLine(text, [&](const std::string& section, const std::string& line, const std::string& key, const std::string&) {
                if (synced(section, key)) std::printf("[sync]   [%s] %s\n", section.c_str(), line.c_str());
            });
        online::postNotice("Using " + sHostName + "'s settings while visiting");
    } else if (logOn()) {
        std::printf("[sync] member %u: %zu skin tones\n", member, tones.size());
    }
}

void matchEnded() {
    std::lock_guard<std::mutex> g(sMutex);
    sTones.clear();
    pushTones();
    if (!sVisiting) return;
    sVisiting = false;
    port::endSettingsOverride();
    std::printf("[sync] visit over: own settings back\n");
    if (logOn())
        eachLine(port::settingsText(), [&](const std::string& section, const std::string& line, const std::string& key, const std::string&) {
            if (synced(section, key) && (section == "calendar" || section == "gameplay")) std::printf("[sync]   [%s] %s\n", section.c_str(), line.c_str());
        });
}

void memberLeft(uint8_t member) {
    std::lock_guard<std::mutex> g(sMutex);
    if (sTones.erase(member)) pushTones();
}

bool locked() {
    std::lock_guard<std::mutex> g(sMutex);
    return sVisiting;
}

std::string lockNote() {
    std::lock_guard<std::mutex> g(sMutex);
    return "Using " + sHostName + "'s settings while visiting";
}

bool lockBegin() {
    if (!locked()) return false;
    ImGui::TextDisabled("%s", lockNote().c_str());
    ImGui::SetItemTooltip("These options shape the shared town. Your own values come back when you are home again.");
    ImGui::BeginDisabled();
    return true;
}

void lockEnd(bool began) {
    if (began) ImGui::EndDisabled();
}

}  // namespace recomp::online::sync
