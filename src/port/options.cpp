#include "port/options.h"

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
            // --input @<file>: the script read from a file (long test scripts exceed the command line).
            if (!o.inputScript.empty() && o.inputScript[0] == '@') {
                if (FILE* f = std::fopen(o.inputScript.c_str() + 1, "rb")) {
                    std::string s;
                    char buf[4096];
                    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) s.append(buf, n);
                    std::fclose(f);
                    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
                    o.inputScript = s;
                } else {
                    std::fprintf(stderr, "--input: cannot read %s\n", o.inputScript.c_str() + 1);
                }
            }
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
