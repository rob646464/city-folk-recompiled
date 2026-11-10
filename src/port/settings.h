#pragma once
// User settings, edited in the in-game overlay menu (F1) and saved to
// %APPDATA%\Animal Crossing City Folk Recompiled\settings.ini. Read them wherever they apply;
// changes take effect immediately unless noted.
#include <functional>
#include <iosfwd>
#include <map>
#include <string>

namespace port {

enum class AspectMode { Original4x3, Auto, Wide16x9, Wide16x10, Ultra21x9, Ultra32x9 };
// Sound output. Stereo / Mono / Surround set the game's own sound mode (its options menu
// shows and changes the same thing); Surround plays the console's matrix-encoded stereo.
// Surround51 / Surround71 run the game in surround mode and play its centre and surround
// buses on their own speakers (when the device has them; else like Surround).
enum class AudioOutput { Stereo, Mono, Surround, Surround51, Surround71 };

constexpr int kMinFps = 30, kMaxFps = 320;

struct VideoSettings {
    // Presentation frame rate. 30 = the game's own rate, drawn directly (no interpolation);
    // above 30 the frames in between are interpolated (src/recomp/interp.*). matchDisplay
    // uses the refresh rate of the display the window is on instead of targetFps.
    int targetFps = 30;         // kMinFps..kMaxFps
    bool unlimitedFps = false;  // interpolated, as fast as the PC can (no frame-rate cap or schedule)
    bool matchDisplay = false;
    AspectMode aspect = AspectMode::Auto;
    int resolutionScale = 0;    // internal render resolution: 0 = match window, N = N x 480p (aspect-widened)
    float fovScale = 1.0f;      // multiplies the game's vertical field of view
    bool fullscreen = false;
    bool vsync = true;          // applied at next launch
    int msaa = 1;               // 1, 2, 4, 8; applied at next launch
    bool hudKeep4x3 = true;     // keep 2D/HUD at 4:3 (centered) in widescreen
    // (No PAL refresh option: the European game's 50/60 Hz is the console's EuRGB60 setting, sys::settings().)
    // Interpolated presentation on a variable refresh rate display (G-SYNC / FreeSync): 0 auto
    // (detected from the display's refresh timing), 1 on, 2 off (frame.cpp, docs/INTERPOLATION.md).
    int vrr = 0;
};

// CRT post-process filter (src/recomp/crt.cpp). Beam sigmas are fractions of the
// scanline spacing, convergence offsets are in game pixels / lines, mask size in
// output pixels, corner radius a fraction of the screen height.
enum class CrtPreset { ApertureGrille, SlotMask, ShadowMask, ConsumerTv, Custom };
enum class CrtScanlines { Progressive480, DoubleStrike240, Interlaced480i };
enum class CrtMask { ApertureGrille, SlotMask, ShadowMask, None };
// Video signal between the console and the tube (crt.cpp "signal path"): RGB is the clean
// picture; the others model the NTSC encoder, the cable / RF channel and the TV's decoder.
enum class CrtSignal { Rgb, SVideo, Composite, Rf };
// How the TV splits a composite signal into luma and chroma.
enum class CrtSeparation { Notch, Comb2Line, Comb3Line };

struct CrtSettings {
    bool enabled = false;
    CrtPreset preset = CrtPreset::ApertureGrille;
    CrtScanlines scanlines = CrtScanlines::Progressive480;
    float beamMinSigma = 0.20f, beamMaxSigma = 0.36f, beamSpotPower = 0.6f;
    float beamMinShape = 2.0f, beamMaxShape = 3.0f, beamShapePower = 0.5f;
    float horizontalSoftness = 0.45f;
    CrtMask mask = CrtMask::ApertureGrille;
    float maskSize = 3.0f, maskStrength = 0.75f;
    float halation = 0.04f, diffusion = 0.06f, bloom = 0.15f;
    float convergence[6] = {0, 0, 0, 0, 0, 0};  // R x,y  G x,y  B x,y
    float gammaIn = 2.4f, gammaOut = 2.2f;
    float curvature = 0.0f, cornerRadius = 0.02f;
    float brightness = 1.2f;
    // Signal path. Bandwidths in MHz of the NTSC baseband; crt::applySignal sets the signal's
    // defaults when a signal is chosen (applying a tube preset keeps them).
    CrtSignal signal = CrtSignal::Rgb;
    CrtSeparation separation = CrtSeparation::Notch;
    float lumaBandwidth = 4.2f;    // TV luma channel
    float chromaBandwidth = 1.0f;  // TV chroma demodulator low-pass
    float sharpening = 0.25f;      // TV luma peaking
    float artifacts = 1.0f;        // Y/C crosstalk (dot crawl, cross-colour): 1 = as modelled, 0 = none
    float saturation = 0.92f;      // TV colour control
    float noise = 0.0f;            // RF snow
    float ghost = 0.0f, ghostDelay = 1.0f;  // multipath echo: gain, delay in microseconds
    float jitter = 0.0f;           // RF line jitter, game pixels
    float hum = 0.0f;              // rolling hum bar depth
    float interference = 0.0f;     // beat with an interfering carrier (diagonal bars)
};

struct AudioSettings {
    float masterVolume = 1.0f;  // 0..1
    float musicVolume = 1.0f;   // music players of the sound archive (BGM, K.K., fanfares, title stream)
    float sfxVolume = 1.0f;     // everything else
    float voiceVolume = 1.0f;   // animal speech (Animalese) players
    float remoteVolume = 1.0f;  // Wii Remote speaker sound played on the PC (0 = off)
    AudioOutput output = AudioOutput::Stereo;
    bool swapChannels = false;
    bool reverb = true;        // aux bus returns (the game's AXFX reverb/chorus/delay) mixed back in
    bool voiceFilters = true;  // per-voice low-pass / biquad (and remote speaker filter) of the AX microcode
    bool sincResampler = true; // windowed-sinc 32 kHz -> device rate (off: SDL's resampler)
};

struct EnhancementSettings {
    bool showFps = false;
    bool pauseOnFocusLoss = true;
};

// Gameplay options (F1 > Enhancements > Gameplay; docs/FEATURES.md). Defaults are the original game.
struct GameplaySettings {
    // Grass wear 0 Original, 1 New Leaf style, 2 Off (src/game/cf_grass.cpp).
    int grassWear = 0;
    // Skin tone per resident (src/game/cf_skin.cpp): "<town id>-<player id>" (hex) -> fixed tone 1..8
    // (absent = the game's tanning). Saved as the [skin_tone] section by cf_skin.cpp.
    std::map<std::string, int> skinTone;
};

// Hardware-accuracy rendering features (F1 > Visuals > Rendering). Every costly accuracy
// feature gets a flag here, ON by default (best quality); weaker PCs can turn them off.
struct RenderingSettings {
    // Integer TEV: the color combiner computes with the hardware's 8-bit inputs, signed 11-bit
    // registers and per-stage rounding instead of floats (Aurora GX_AURORA_ACCURACY_INT_TEV).
    bool integerTev = true;
    // EFB color depth: pixels are stored the way the game's RGBA6 framebuffer stores them,
    // 6 bits per channel with the ordered dither the game enables (GX_AURORA_ACCURACY_EFB_DEPTH).
    bool efbColorDepth = true;
    // 6-bit blending: with efbColorDepth, blended draws (the flashlight beam, glows, HUD edges)
    // are stored like the console's RGBA6 framebuffer stores them, the blend result dithered and
    // truncated to 6 bits (GX_AURORA_ACCURACY_EFB_BLEND, Aurora 0176).
    bool efbBlend6 = true;
    // Smooth color banding (enhancement, OFF = vanilla-accurate): with efbColorDepth, the 6-bit
    // framebuffer and the game's RGB565 screen copy store the local average of the console's
    // 6/5-bit result instead of its steps, so gradients keep the same brightness and tint without
    // contour bands or dither grain, plus a sub-step noise so the 8-bit target does not band either
    // (Aurora 0190, GX_AURORA_ACCURACY_EFB_SMOOTH + _NOISE). Forced on while enhanced lighting is on
    // (its per-pixel gradients and bloom band badly in 6 bits).
    bool smoothBanding = false;
    // Early Z writes depth for alpha-tested pixels: draws that compare Z before texturing
    // (GXSetZCompLoc(GX_TRUE)) update Z even where the alpha test then rejects the pixel, as the
    // hardware does (GX_AURORA_ACCURACY_EARLY_Z, Aurora 0170/0179: depth-only pass + colour pass).
    bool earlyZAlphaDepth = true;
    // Display copy filter: the vertical filter (and gamma) GXCopyDisp applies on the way to the TV.
    bool displayCopyFilter = true;
    // Texture copy filter: the same vertical filter applied to the game's GXCopyTex copies (the
    // full-screen RGB565 copy it redraws every frame, the box-filtered half-size copies...), as on
    // the console (GX_AURORA_ACCURACY_COPY_FILTER).
    bool textureCopyFilter = true;
    // GXCopyTex results also exist as GX-format bytes in main memory, so textures that read a
    // copy with another format/size (the shadow depth map) and CPU reads see what the
    // console sees (docs/RENDERING.md). Off: copies stay GPU-only textures.
    bool efbCopyToRam = true;
    // Vertex data each draw indexes is copied when the game issues the draw (as the console's GPU
    // reads it then), not when Aurora's FIFO thread gets to it; fixes Luigi's rare mouth/hose
    // flicker (CPU/GPU timing race; docs/RENDERING.md "Vertex snapshots"). Interpolated frame
    // rates snapshot anyway.
    bool vtxSnapshot = true;
};

// Enhanced lighting (F1 > Visuals > Enhanced lighting; docs/RENDERING.md "Enhanced lighting"):
// effects layered on top of the accurate GX rendering of world geometry. Everything is gated by
// `enabled` (OFF by default: the original look stays one click away; off renders exactly what
// the plain renderer does). Each part has its own switch and strength/quality controls; the
// preset fills in the quality fields (Custom keeps them).
enum class EnhancedPreset { Low, Medium, High, Ultra, Custom };

struct EnhancedLightingSettings {
    bool enabled = false;
    EnhancedPreset preset = EnhancedPreset::High;
    // The game's own GX lights (room lights, flashlight, specular) evaluated per pixel.
    bool perPixelLighting = true;
    // Flashlight cone per pixel with a smooth penumbra.
    bool flashlightQuality = true;
    float flashlightSoftness = 0.6f;  // 0..1
    float flashlightSpecular = 0.6f;  // 0..2: glossy highlight where the beam hits (per pixel)
    // Soft shadows for the game's shadow map (PCF / PCSS contact hardening).
    bool softShadows = true;
    int softShadowQuality = 2;        // 0 PCF 8, 1 PCF 16, 2 PCSS 16, 3 PCSS 32
    float softShadowRadius = 1.0f;    // filter radius (640x480 shadow-map texels)
    float softShadowPenumbra = 6.0f;  // PCSS penumbra growth
    // Shadow look: the game darkens shadowed pixels toward black whatever lights them. With these
    // (enhanced shaders only) colored lights keep lighting the shadow, part of the ambient light is
    // kept (shadows pick up the room's color), and the darkness is scaled. 1 / 0 = the game's look.
    float shadowDarkness = 0.9f;      // 0..1.5
    float shadowAmbientKeep = 0.25f;  // 0..1 ("Colored shadow tint")
    int shadowMapSize = 0;            // 0 render resolution, 1 2048x1536, 2 4096x3072 (offscreen)
    // Dynamic colored lights from game objects (ghosts) and test lights.
    bool extraLights = true;
    float extraLightIntensity = 1.0f;  // 0..3
    float extraLightSaturation = 1.6f; // 0..2.5: color vibrance of all colored lights (1 = as the source gives it)
    bool extraLightShadows = true;     // shadows of lights flagged to cast them (ghost lights: the strongest two)
    int extraShadowQuality = 1;        // 0 low, 1 medium, 2 high (ray-march steps / shadow-map filter taps)
    int extraShadowMode = 1;           // 0 screen-space (cheap), 1 shadow maps (cube maps per light)
    int extraShadowLights = 2;         // 0..5: 0 / 2 / 4 / 8 / 12 / 16 shadow-casting lights per frame
    int extraShadowMapSize = 1;        // 0..3: 256 / 512 / 1024 / 2048 texels per cube face
    float extraShadowSoftness = 14.0f; // light size (world units) for soft contact-hardening edges; 0 = hard
    // Volumetric haze in the flashlight beam.
    bool volumetricBeam = true;
    float volumetricIntensity = 2.0f;  // 0..3
    int volumetricQuality = 1;         // 0 low, 1 medium, 2 high
    // Realistic flashlight (Aurora 0196; replaces the volumetric beam while on): light scattering
    // through the air against the flashlight's shadow map (shadow shafts), reflector-shaped
    // footprint with inverse-square falloff, dust motes, the game's cone mesh hidden.
    int realisticFlashlight = 2;       // 0 off, 1 medium, 2 high, 3 ultra
    float beamDensity = 1.0f;          // 0..3: haze in the air
    float beamDust = 1.0f;             // 0..2: dust motes in the beam
    // Glow around bright light sources (HUD excluded).
    bool bloom = true;
    float bloomIntensity = 0.5f;       // 0..2
    float bloomThreshold = 0.8f;       // 0..1
    float bloomRadius = 1.0f;          // 0.25..2
    float bloomFlashlightSuppress = 0.6f;  // 0..1: less glow from walls lit by the flashlight
    float bloomScene = 0.12f;          // 0..1: glow of ordinary bright surfaces (above the threshold)
    float bloomEmissive = 0.6f;        // 0..2: glow of light-emitting effects (glows, fire, beams, sparks)
    // Screen-space ambient occlusion (contact darkening in corners and under furniture;
    // horizon-based since Aurora 0224). Off by default since branch stutter-ao (user: it
    // overpowered the real-time shadows); since Aurora 0450 it only darkens the ambient share of
    // the light and the intensity is visibility^(2 x intensity) (0.5 = the plain occlusion).
    bool ssao = false;
    float ssaoIntensity = 0.3f;        // 0..2
    float ssaoRadius = 40.0f;          // world units
    // Version of the AO settings above (settings.cpp loadSettings): files without it were written
    // with AO on by default and the old curve, and are migrated once (logged).
    int aoVersion = 2;

    // ---- City Folk (src/game/cf_light.cpp, docs/RENDERING.md "Enhanced lighting (City Folk)") ----
    // Sun and moon: soft shadow maps from a directional light that follows the game clock.
    bool sunShadows = true;
    int sunDirection = 0;              // 0 time of day (real sun path), 1 the game's own key light
    float sunShadowStrength = 0.6f;    // 0..1: how much a full shadow darkens (day; the moon's is weaker)
    float sunShadowDistance = 3500.0f; // world units of view depth with shadows (a town acre is 640)
    float sunShadowSoftness = 1.0f;    // 0..3: penumbra (the light's apparent size)
    int sunShadowMapSize = 1;          // 0..2: 512 / 1024 / 2048 texels per cascade (preset)
    int sunShadowQuality = 1;          // 0..2: filter taps 8 / 12 / 20 (preset)
    bool sunLight = true;              // warm sunlight added on sun-facing surfaces (shadowed)
    float sunLightIntensity = 0.3f;    // 0..1
    int blobShadows = 1;               // the game's round blob shadows: 0 keep, 1 fade under real shadows, 2 hide
    bool indoorShadows = true;         // rooms: shadows from the room's main light
    // Time of day: golden hour, blue dusk, cool moonlight, from the game clock (and the weather).
    bool timeOfDay = true;
    float timeOfDayStrength = 1.0f;    // 0..2
    bool timeOfDayWeather = true;      // overcast / rain / snow flatten the light
    // Night lights: windows, lamps, shop interiors, fireflies, fireworks light their surroundings.
    bool nightLights = true;
    float nightLightIntensity = 1.0f;  // 0..3
    bool nightLightShadows = true;     // the strongest night lights cast shadows
    bool nightLightsGameState = true;  // windows follow the game's own lights-on state (off: every building glows at night)
    bool effectLights = true;          // fireflies and fireworks are small coloured lights (game particle positions/colours)
    // Furniture lights (src/game/cf_furniture_light.cpp): lamps, TVs, fires, candles... light the
    // room, following the game's own on/off state.
    bool furnitureLights = true;
    float furnitureLightIntensity = 1.0f;  // 0..3
    bool furnitureLightShadows = true;     // fires, screens and lamps (from below their shade) cast shadows
    // 0..1: how much of the game's own whole-room brightening a lamp or candle switched on keeps
    // (dEnvAmbLight_c's room light; the rest of the light comes from the lamp itself). 1 = the game's.
    float furnitureRoomFill = 0.0f;
    // 0..2.5: colour vibrance of furniture lights (HSV saturation x this, up to full; 1 = the
    // source's colour). Default punchy, like LM's "Light color vibrance".
    float furnitureVibrance = 1.6f;
    // Sun shafts: light scattered in the air, cut by the shadows of trees and houses.
    bool sunShafts = true;
    float sunShaftIntensity = 1.0f;    // 0..3
    int sunShaftQuality = 1;           // 0..2: march steps 12 / 20 / 32 (preset)
};

// Self-updater (F1 > Game > Updates; src/port/update.cpp, docs/SETUP.md "Updates").
struct UpdateSettings {
    bool checkAtStartup = true;  // look for a newer GitHub release in the background at startup
    std::string skipVersion;     // release tag the player chose "Skip this version" for ("v1.2")
};

// Size of the F1 menu and the port's other overlays (src/recomp/ui_scale.cpp; issue #20).
struct InterfaceSettings {
    int menuScale = 0;  // percent (75..300, ui::kSteps); 0 = Auto (window height / display scaling)
    bool startHint = true;  // "Press F1 for options" notice for the first seconds of play (menu.cpp)
};

// Nintendo event distributions (F1 > Online > Nintendo events; src/recomp/distributions.cpp,
// docs/DISTRIBUTIONS.md). Off by default: nothing is downloaded and WiiConnect24 stays off.
struct NintendoEventSettings {
    int mode = 0;               // 0 off, 1 loop (every distribution in turn), 2 original dates, 3 only when I choose
    int loopPeriod = 0;         // loop: 0 every week, 1 every 2 weeks, 2 every month
    bool originalYearly = true; // original dates: every year (month/day); false = only in the original years
    bool otherRegions = false;  // include other regions' distributions
    bool liveDelivery = true;   // USA: run the game's own WiiConnect24 check while playing
    bool townWc24 = true;       // USA: turn the town's own WiiConnect24 setting on (the game's setter) when it is off
};

// Online play between players of this port (F1 > Online; src/recomp/online.cpp, docs/ONLINE.md).
// Room passwords are never saved.
struct OnlineSettings {
    std::string playerName;     // shown to the others in a room; empty = "Player"
    std::string roomName;       // empty = "<name>'s town"
    int port = 29910;           // UDP port when hosting (1024..65535)
    bool askBeforeJoin = false; // the host accepts each joiner
    bool allowDirect = true;    // joiners that know only IP:port (no room code) may join
    bool lanVisible = true;     // announce the room on the local network
    bool portMapping = true;    // UPnP / NAT-PMP mapping on the router + public address via STUN
    std::string lastDirect;     // last "Direct connect" address
    bool notices = true;        // room events as notices over the game
    // "Always online mail" (online_email.cpp): own account only; the password is in the OS credential store.
    bool emailOn = false;
    std::string emailAddress, emailPreset;
    std::string emailImapHost, emailSmtpHost;
    int emailImapPort = 993, emailSmtpPort = 465;
    std::string relay;          // optional self-hosted relay "host[:port]" (tools/relay/cf_relay.cpp)
    bool usbKeyboard = false;   // the PC keyboard is the Wii USB keyboard (chat); applies when the game opens it
    // Wii Speak from the PC microphone (src/recomp/online_voice.cpp). Nothing is recorded or sent while off.
    bool micOn = false;
    std::string micDevice;      // SDL recording device name; empty = the system default
    bool micPushToTalk = true;  // true: send only while the push-to-talk key/button is held; false: open mic
    int micPttKey = 25;         // SDL scancode (25 = V); 0 = none
    int micPttPadButton = -1;   // SDL gamepad button on port 1's controller; -1 = none
    float micGain = 1.0f;       // input gain, 0..4
    bool micEchoSuppression = true;  // the game's Wii Speak echo suppressor (half duplex); off = full duplex (headphones)
};

// Move in from Wild World (F1 > Enhancements > Gameplay; src/game/cf_move_in.cpp, docs/MOVE_IN.md):
// Rover's "Move from the DS!" reads a Wild World save file through the game's own DS receive path.
struct MoveInSettings {
    bool anyRegion = false;  // accept Wild World saves of every region (a Wii accepts only its own region's)
    std::string lastFile;    // the save file picked last time (offered again in the window)
};

// F1 > Enhancements > Unused content (src/game/cf_unused_items.cpp, docs/FEATURES.md "Unused items
// at Nook's"): every option off by default.
struct UnusedContentSettings {
    // The one unused item Nook's sells in place of one of the day's items of its category (item
    // code, e.g. 0xC0AC Nintendo bench; 0 = none, retail stock).
    int nookItem = 0;
};

struct Settings {
    InterfaceSettings ui;  // (not "interface": a macro in the Windows headers)
    MoveInSettings moveIn;
    UnusedContentSettings unusedContent;
    VideoSettings video;
    RenderingSettings rendering;
    EnhancedLightingSettings enhancedLighting;
    CrtSettings crt;
    AudioSettings audio;
    EnhancementSettings enhancements;
    GameplaySettings gameplay;
    UpdateSettings updates;
    NintendoEventSettings events;
    OnlineSettings online;
};

Settings& settings();
void loadSettings();
void saveSettings();
// Every setting (the table and all extensions, e.g. controller bindings) back to its first-install
// default, then saved.
void resetSettingsToDefaults();
std::string settingsPath();
// Sets one ini key of the table (as if read from settings.ini, then sanitized); false when the
// key is unknown. Not saved. Used by the CF_SETTINGS_AT test aid (menu.cpp).
bool setSetting(const std::string& section, const std::string& key, const std::string& value);

// Settings as ini text (what saveSettings writes), and applying ini text (as if read from
// settings.ini) to the keys `want(section, key)` accepts; no file is touched.
std::string settingsText();
void applySettingsText(const std::string& text, const std::function<bool(const std::string&, const std::string&)>& want);
// Temporary override (online visits, docs/ONLINE.md "Settings while visiting"): the keys `synced`
// accepts take their values from `text` (the host's) until endSettingsOverride() puts this
// player's own values back. While active, saveSettings writes this player's own values for those
// keys, so settings.ini never holds the host's.
void beginSettingsOverride(const std::string& text, const std::function<bool(const std::string&, const std::string&)>& synced);
void endSettingsOverride();
bool settingsOverridden();

// Ini sections owned by another module (e.g. controller bindings, src/recomp/controls.cpp).
// reset() runs before every load, load() gets each key the table above does not know,
// save() appends the module's own sections. Register from a static initializer.
struct SettingsExtension {
    void (*reset)();
    void (*load)(const std::string& section, const std::string& key, const std::string& value);
    void (*save)(std::ostream& out);
};
void addSettingsExtension(const SettingsExtension& ext);

}  // namespace port
