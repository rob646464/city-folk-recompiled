// macOS window helpers (docs/MACOS.md). Kept apart from the runtime's other files: the
// Objective-C runtime headers' BOOL clashes with the Dolphin SDK's.
#ifdef __APPLE__
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>
#include <objc/message.h>
#include <objc/runtime.h>

namespace recomp {

// CF_OFFSCREEN=1: macOS keeps every window on a display, so the window is made fully
// transparent and click-through instead; it still renders and presents.
void hideWindowMacos(SDL_Window* w) {
    SDL_SetWindowOpacity(w, 0.0f);
    if (void* ns = SDL_GetPointerProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr))
        reinterpret_cast<void (*)(void*, SEL, BOOL)>(objc_msgSend)(ns, sel_registerName("setIgnoresMouseEvents:"), YES);
}

}  // namespace recomp
#endif
