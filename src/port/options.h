#pragma once
// Command-line options shared between the host and game code.
#include <cstdint>
#include <string>

namespace port {

struct Options {
    std::string disc;         // --disc <path>
    std::string modelViewer;  // --model-viewer <name>: dev model viewer instead of the title screen
    bool viewerRestPose = false;  // --viewer-rest: ignore the model's animation
    std::string roomViewer;   // --room-viewer <map>/<room>: e.g. map2/room_01
    std::string mapViewer;    // --map-viewer <map>: every room of a map with a fly camera (e.g. map2)
    // --input "<retrace>:<BUTTON>[+BUTTON...][*frames],...": scripted controller 1 input
    // for automated testing, e.g. "300:START,420:A*10,500:STICKUP*60".
    std::string inputScript;

    // --aspect auto|4:3|16:9|21:9|<w>:<h>|<ratio> (recompiled build; docs/ENHANCEMENTS.md)
    //   auto (default): the 3D view widens to the window's shape (Hor+); 2D stays 4:3, centered.
    //   4:3: the original presentation, pillarboxed in a wider window.
    //   anything else: a fixed aspect; the initial window is sized to match.
    // 0 means auto, otherwise width / height.
    float aspectRatio = 0.0f;
    // --ws-backgrounds pillarbox|stretch|zoom: full-screen 4:3 pictures in widescreen
    // (0 = keep 4:3 with side bars, 1 = stretch to the window, 2 = enlarge and crop).
    int wsBackgrounds = 0;
    std::uint32_t windowWidth = 0, windowHeight = 0;  // --window <w>x<h>
    bool fullscreen = false;                          // --fullscreen
    // --hidden-mansion-variant pal|us (alias --hidden-mansion): initial Hidden Mansion variant,
    // i.e. which release's rules apply while the game's hidden-mansion flag is set (default
    // pal; docs/HIDDEN_MANSION.md).
    std::string hiddenMansion;  // empty: use the F1 menu setting
};

Options& options();
void parseOptions(int argc, char* argv[]);

// Parses "16:9", "1.777" or "auto" (-> 0). Returns a negative value if malformed.
float parseAspect(const char* s);

}  // namespace port
