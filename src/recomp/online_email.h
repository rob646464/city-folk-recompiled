#pragma once
// "Always online mail" (docs/ONLINE.md "Letters: A. email"): letters to friends as encrypted, signed
// attachments over the player's own email account (SMTP out, IMAP in), the way WiiConnect24 carried
// them. Off by default; the password lives only in the OS credential store.
#include <string>
#include <vector>

namespace recomp::online::email {

struct Preset {
    const char* name;
    const char* imapHost;
    int imapPort;
    const char* smtpHost;
    int smtpPort;   // 465 implicit TLS, 587 STARTTLS
    bool supported;  // false: the provider allows no password IMAP/SMTP (OAuth only)
    const char* note;
};
const std::vector<Preset>& presets();

// Password in the OS credential store (Windows Credential Manager). False when the system has none
// this build can use (then nothing is stored).
bool storePassword(const std::string& address, const std::string& password, std::string* err);
bool havePassword(const std::string& address);
void forgetPassword(const std::string& address);

// F1 > Online "Always online mail" section; true when a setting changed.
bool drawSection();

// Host thread, each frame: starts / stops the background worker as the setting says.
void tick();

// Test aid (CF_ONLINE_TEST=<f>:mailself): a test letter to one's own address.
void sendTestLetter();

}  // namespace recomp::online::email
