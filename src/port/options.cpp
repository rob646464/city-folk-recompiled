#include "port/options.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace port {

Options& options() {
    static Options sOptions;
    return sOptions;
}

float parseAspect(const char* s) {
    if (std::strcmp(s, "auto") == 0) return 0.0f;
    char* end = nullptr;
    const double a = std::strtod(s, &end);
    if (end == s || a <= 0.0) return -1.0f;
    if (*end == ':' || *end == '/' || *end == 'x') {
        const char* bs = end + 1;
        const double b = std::strtod(bs, &end);
        if (end == bs || b <= 0.0 || *end) return -1.0f;
        return float(a / b);
    }
    return *end ? -1.0f : float(a);
}

namespace {

bool readFile(const std::string& path, std::string& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) out.append(buf, n);
    std::fclose(f);
    return true;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}

// Route files (tools/routes/*.txt): one or more items per line separated by commas, `#` starts a
// comment, `include <file>` (relative to the including file) inserts another route. An item's
// time may be `+N`: N after the previous item's start (in the same clock), so routes can be
// edited and chained without renumbering. Output: the plain comma list the parsers read.
std::string anchor;  // current `after link` module ("" = absolute times)
unsigned long anchorCount = 1, anchorLast = 0;

void expandScript(const std::string& text, const std::string& dir, std::string& out, unsigned long& last, int depth) {
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        line = trim(line);
        if (line.empty()) continue;
        if (line.rfind("include ", 0) == 0) {
            std::string path = trim(line.substr(8));
            const bool absolute = path.size() > 1 && (path[0] == '/' || path[0] == '\\' || path[1] == ':');
            if (!absolute) path = dir + path;
            std::string sub;
            if (depth < 8 && readFile(path, sub)) {
                const auto slash = path.find_last_of("/\\");
                expandScript(sub, slash == std::string::npos ? std::string() : path.substr(0, slash + 1), out, last,
                             depth + 1);
            } else {
                std::fprintf(stderr, "--input: cannot include %s\n", path.c_str());
            }
            continue;
        }
        // `after link <REL module> [n]`: the following items are timed from the n-th (default 1st)
        // time the game links that module (a scene change: d_bgi_drawNP = indoors), +N relative,
        // until an item with an absolute time. Emitted as E<module>.<n>.<offset>:<tokens>.
        if (line.rfind("after link ", 0) == 0) {
            std::string rest = trim(line.substr(11));
            const auto sp = rest.find(' ');
            anchor = rest.substr(0, sp);
            anchorCount = sp == std::string::npos ? 1 : std::max(1ul, std::strtoul(rest.c_str() + sp + 1, nullptr, 10));
            anchorLast = 0;
            continue;
        }
        size_t ip = 0;
        while (ip <= line.size()) {
            size_t comma = line.find(',', ip);
            if (comma == std::string::npos) comma = line.size();
            std::string item = trim(line.substr(ip, comma - ip));
            ip = comma + 1;
            if (item.empty()) continue;
            std::string prefix;
            if (item.size() > 3 && (item[0] == 'P' || item[0] == 'p') && item[1] >= '1' && item[1] <= '4' && item[2] == ':') {
                prefix = item.substr(0, 3);
                item.erase(0, 3);
            }
            // `~N` at the end repeats the item N times; a +D time spaces the copies D apart.
            unsigned long repeat = 1;
            if (const auto tilde = item.rfind('~'); tilde != std::string::npos) {
                repeat = std::max(1ul, std::strtoul(item.c_str() + tilde + 1, nullptr, 10));
                item = trim(item.substr(0, tilde));
            }
            const auto colon = item.find(':');
            if (colon == std::string::npos) continue;
            const std::string t = trim(item.substr(0, colon));
            const bool relative = !t.empty() && t[0] == '+';
            const unsigned long v = std::strtoul(t.c_str() + (relative ? 1 : 0), nullptr, 10);
            if (!relative) anchor.clear();  // an absolute time ends an `after link` block
            for (unsigned long r = 0; r < repeat; r++) {
                if (!out.empty()) out += ',';
                if (!anchor.empty()) {
                    anchorLast += v;
                    out += prefix + "E" + anchor + "." + std::to_string(anchorCount) + "." + std::to_string(anchorLast) +
                           item.substr(colon);
                    continue;
                }
                const unsigned long at = relative ? last + v : (r == 0 ? v : last);
                last = at;
                out += prefix + std::to_string(at) + item.substr(colon);
            }
        }
    }
}

}  // namespace

std::string expandInputScript(const std::string& text, const std::string& dir) {
    std::string out;
    unsigned long last = 0;
    expandScript(text, dir, out, last, 0);
    return out;
}

void parseOptions(int argc, char* argv[]) {
    Options& o = options();
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--viewer-rest") == 0) {
            o.viewerRestPose = true;
            continue;
        }
        if (std::strcmp(argv[i], "--fullscreen") == 0) {
            o.fullscreen = true;
            continue;
        }
        if (i + 1 >= argc) {
            break;
        }
        if (std::strcmp(argv[i], "--disc") == 0) {
            o.disc = argv[++i];
        } else if (std::strcmp(argv[i], "--model-viewer") == 0) {
            o.modelViewer = argv[++i];
        } else if (std::strcmp(argv[i], "--map-viewer") == 0) {
            o.mapViewer = argv[++i];
            o.modelViewer = "(map)";
        } else if (std::strcmp(argv[i], "--hidden-mansion") == 0 ||
                   std::strcmp(argv[i], "--hidden-mansion-variant") == 0) {
            o.hiddenMansion = argv[++i];
        } else if (std::strcmp(argv[i], "--input") == 0) {
            o.inputScript = argv[++i];
            // --input @<file>: the script read from a route file (lines, comments, includes, +N
            // relative times; tools/routes/). Plain scripts may use +N too.
            if (!o.inputScript.empty() && o.inputScript[0] == '@') {
                const std::string path = o.inputScript.substr(1);
                std::string s;
                if (readFile(path, s)) {
                    const auto slash = path.find_last_of("/\\");
                    o.inputScript = expandInputScript(s, slash == std::string::npos ? std::string() : path.substr(0, slash + 1));
                } else {
                    std::fprintf(stderr, "--input: cannot read %s\n", path.c_str());
                }
            } else {
                o.inputScript = expandInputScript(o.inputScript, std::string());
            }
            if (std::getenv("CF_INPUT_LOG")) std::fprintf(stderr, "[input] script: %s\n", o.inputScript.c_str());
        } else if (std::strcmp(argv[i], "--room-viewer") == 0) {
            o.roomViewer = argv[++i];
            o.modelViewer = "(room)";  // the viewer scene handles both
        } else if (std::strcmp(argv[i], "--aspect") == 0) {
            const float a = parseAspect(argv[++i]);
            if (a < 0.0f) std::fprintf(stderr, "Ignoring --aspect %s (expected auto, 16:9 or a ratio)\n", argv[i]);
            else o.aspectRatio = a;
        } else if (std::strcmp(argv[i], "--ws-backgrounds") == 0) {
            const char* v = argv[++i];
            if (std::strcmp(v, "pillarbox") == 0) o.wsBackgrounds = 0;
            else if (std::strcmp(v, "stretch") == 0) o.wsBackgrounds = 1;
            else if (std::strcmp(v, "zoom") == 0) o.wsBackgrounds = 2;
            else std::fprintf(stderr, "Ignoring --ws-backgrounds %s (pillarbox, stretch or zoom)\n", v);
        } else if (std::strcmp(argv[i], "--window") == 0) {
            unsigned w = 0, h = 0;
            if (std::sscanf(argv[++i], "%ux%u", &w, &h) == 2 && w && h) {
                o.windowWidth = w;
                o.windowHeight = h;
            } else {
                std::fprintf(stderr, "Ignoring --window %s (expected <width>x<height>)\n", argv[i]);
            }
        }
    }
}

}  // namespace port
