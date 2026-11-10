// Nintendo event distributions (distributions.h; docs/DISTRIBUTIONS.md).
//
// Content: WiiLink's archive of the City Folk distribution items, designs and letters (the
// ACDLCPicker repository; used with WiiLink's permission). It is not part of this repository:
// the files are downloaded at run time from a pinned commit, checked against the SHA-1 values
// below and cached in <data folder>/distributions/wiilink-archive/. Original WiiConnect24 files
// (.enc) the player puts into <data folder>/distributions/ are served as they are and go through
// KD's signature check (nwc24_dl.cpp).
//
// Dates, regions and themes: Nookipedia's list of City Folk distributions (as data; dates are
// facts, the list itself is not reproduced). Region letters: E = North America, P = Europe,
// J = Japan, K = South Korea.
#include "recomp/distributions.h"

#include "port/http.h"
#include "port/install.h"
#include "port/settings.h"
#include "recomp/distrib_record.h"
#include "recomp/hle.h"
#include "recomp/menu.h"
#include "recomp/nwc24_dl.h"
#include "recomp/online.h"
#include "recomp/runtime.h"
#include "recomp/system.h"
#include "recomp/town_id.h"
#include "recomp/version.h"
#include "recomp/wc24_crypto.h"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace recomp::distrib {
namespace fs = std::filesystem;

namespace {

// ---- the archive ---------------------------------------------------------------------------

constexpr const char* kArchiveBase = "https://raw.githubusercontent.com/WiiLink24/ACDLCPicker/402566bca6f78106f4e6e021312c94cd6ad4d1ee/";

struct ArchiveFile {
    const char* path;  // in the repository
    const char* sha1;
    u32 size;
};
// Every file of the archive this port uses (items: item.bin of a distribution, 8 KiB "BITM";
// designs: design.bin; stuff: the distribution's English letter as text).
constexpr ArchiveFile kArchive[] = {
    {"items/Blue_Falcon.bin", "3c679b42afad3719b3da069520dbf18ecbd623e4", 8192},
#include "recomp/distributions_archive.inc"
};

// ---- the catalogue ---------------------------------------------------------------------------

enum Type : u8 { Furniture, Headwear, Accessory, Umbrella, Wallpaper, Flooring, Design, Other };
const char* typeName(Type t) {
    static const char* n[] = {"furniture", "headwear", "accessory", "umbrella", "wallpaper", "flooring", "design", "other"};
    return n[t];
}

struct Def {
    const char* key;     // stable id (state file)
    const char* name;    // English name
    Type type;
    const char* item;    // archive item file (items/...), nullptr = not in the archive
    const char* design;  // archive design file (designs/...), sent together with the item
    const char* letter;  // archive letter (stuff/<letter>.txt)
    int paper;           // stationery 0-67 (letter paper id 400 + n)
    const char* dates;   // "E2009-03-25/2009-04-01 P..." (first window per region = its month)
    bool seasonal;       // themed for its time of year (loop schedule keeps it in its month)
    bool unreleased;     // in the archive, never distributed
};

// Stationery numbers (the game's paper list order): the ones used below.
enum Paper : int {
    pButterfly = 0, pMapleLeaf = 7, pLined = 9, pFlowery = 10, pPolkaDot = 11, pRibbon = 13, pSnowman = 17,
    pCow = 20, pHamburger = 22, pFourLeaf = 27, pElegant = 38, pTownView = 39, pChinese = 40, pMushroom = 45,
    pStar = 46, pCool = 50, pForest = 51, pButtercup = 53, pPlaid = 55, pRainbow = 35, pBejeweled = 58,
    pSouthwest = 60, pHalloween = 64, pLovely = 32, pCloudy = 4, pPetal = 5, pSnowy = 6, pTartan = 54,
};

const Def kDefs[] = {
    {"red_pikmin", "red Pikmin", Headwear, "red_Pikmin_hat", nullptr, "red_Pikmin_hat", pFlowery,
     "J2008-12-24/2008-12-31 P2009-02-06/2009-02-19 E2009-03-25/2009-04-01", false, false},
    {"top", "top", Furniture, nullptr, nullptr, nullptr, 0, "J2009-01-01/2009-01-15", true, false},
    {"chocolate_heart", "chocolate heart", Furniture, nullptr, nullptr, nullptr, 0, "J2009-02-14/2009-02-26", true, false},
    {"girls_day_updo", "Girl's Day updo", Headwear, nullptr, nullptr, nullptr, 0, "J2009-03-03/2009-03-15 J2010-02-03/2010-02-12", true, false},
    {"tokonoma", "tokonoma", Furniture, nullptr, nullptr, nullptr, 0, "J2009-03-17/2009-04-02", false, false},
    {"shamrock_hat", "shamrock hat", Headwear, "shamrock_hat", nullptr, "shamrock_hat", pRibbon,
     "E2009-03-17/2009-03-24 P2009-03-17/2009-03-30", true, false},
    {"dsi_b", "Nintendo DSi B", Furniture, "Nintendo_DSi_B", nullptr, "Nintendo_DSi_B", pCool, "E2009-04-05/2009-04-13", false, false},
    {"dsi_w", "Nintendo DSi W", Furniture, "Nintendo_DSi_W", nullptr, "Nintendo_DSi_W", pCool,
     "P2009-04-03/2009-04-13 E2010-04-01/2010-04-30", false, false},
    {"student_cap", "student cap + student uniform", Headwear, nullptr, "student", nullptr, 0, "J2009-04-04/2009-05-01", false, false},
    {"bells_1000", "1,000 Bells", Other, nullptr, nullptr, nullptr, 0, "E2009-04-14/2009-04-29", false, false},
    {"flamenco_hat", "flamenco hat + flamenco dress", Headwear, "flamenco_hat", "flamenco", "flamenco_hat", pButtercup,
     "P2009-04-24/2009-05-03", false, false},
    {"carp_banner", "carp banner", Furniture, nullptr, nullptr, nullptr, 0, "J2009-05-03/2009-05-31", true, false},
    {"bus_model", "bus model", Furniture, "bus_model", nullptr, "bus_model", pTownView, "E2009-05-05/2009-05-19 P2009-05-05/2009-05-15", false, false},
    {"green_headgear", "green headgear", Headwear, "green_headgear", nullptr, "green_headgear", pStar, "P2009-05-22/2009-06-05", false, false},
    {"mayors_flag", "Mayor's flag (design)", Design, nullptr, "mayor", nullptr, 0, "E2009-06-12/2009-06-21 E2010-06-01/2010-06-30", true, false},
    {"ghost_umbrella", "ghost umbrella", Umbrella, nullptr, nullptr, nullptr, 0, "J2009-06-15/2009-07-05", true, false},
    {"sand_castle", "sand castle", Furniture, "sand_castle", nullptr, "sand_castle", pCloudy, "P2009-06-21/2009-07-03", true, false},
    {"ladder_shades", "ladder shades", Accessory, "ladder_shades", nullptr, "ladder_shades", pButtercup,
     "E2009-06-23/2009-06-30 E2009-08-03/2009-08-12", true, false},
    {"hot_dog_hat", "hot dog hat", Headwear, "hot_dog_hat", nullptr, "hot_dog_hat", pHamburger, "E2009-07-03/2009-07-18", true, false},
    {"eiffel_tower", "Eiffel Tower", Furniture, "Eiffel_Tower", nullptr, "Eiffel_Tower", pTownView, "P2009-07-14/2009-07-27", true, false},
    {"morning_glory", "morning glory", Furniture, nullptr, nullptr, nullptr, 0, "J2009-07-20/2009-08-13", true, false},
    {"dolphin_model", "Dolphin model", Furniture, "Dolphin_model", nullptr, "Dolphin_model", 61, "E2009-07-23/2009-08-03", false, false},
    {"gamecube_dresser", "GameCube dresser", Furniture, "GameCube_dresser", nullptr, "GameCube_dresser", pTartan,
     "P2009-08-01/2009-08-31 E2009-09-15/2009-09-23", false, false},
    {"eggplant_cow", "eggplant cow", Furniture, nullptr, nullptr, nullptr, 0, "J2009-08-15/2009-09-15", true, false},
    {"hopscotch_floor", "hopscotch floor", Flooring, "hopscotch_floor", nullptr, "hopscotch_floor", pLined, "E2009-08-15/2009-08-29", false, false},
    {"dirndl_dress", "dirndl dress (design)", Design, nullptr, "dirndl", nullptr, 0, "P2009-09-19/2009-10-02", true, false},
    {"celebration_hat", "celebration hat + red padded vest", Headwear, nullptr, "redchar", nullptr, 0, "J2009-09-21/2009-10-07", false, false},
    {"pile_of_leaves", "pile of leaves", Furniture, "pile_of_leaves", nullptr, "pile_of_leaves", pMapleLeaf, "E2009-09-24/2009-10-09", true, false},
    {"labrador_model", "Labrador model", Furniture, "Labrador_model", nullptr, "Labrador_model", pFourLeaf, "P2009-10-07/2009-10-13", false, false},
    {"red_team_hat", "red team hat", Headwear, nullptr, nullptr, nullptr, 0, "J2009-10-12/2009-10-25", true, false},
    {"white_team_hat", "white team hat", Headwear, nullptr, nullptr, nullptr, 0, "J2009-10-12/2009-10-25", true, false},
    {"chihuahua_model", "Chihuahua model", Furniture, "Chihuahua_model", nullptr, nullptr, pPolkaDot, "P2009-10-14/2009-10-20", false, false},
    {"dachshund_model", "dachshund model", Furniture, "dachshund_model", nullptr, "dachshund_model", pElegant, "P2009-10-21/2009-10-27", false, false},
    {"dalmatian_model", "dalmatian model", Furniture, "dalmatian_model", nullptr, "dalmatian_model", pCow, "P2009-10-28/2009-11-03", false, false},
    {"election_poster", "election poster", Furniture, "election_poster", nullptr, "election_poster", pLined, "E2009-10-29/2009-11-12", true, false},
    {"anniversary_cake", "anniversary cake", Furniture, "anniversary_cake", nullptr, "anniversary_cake", pBejeweled,
     "E2009-11-14/2009-11-24 J2009-11-20/2009-12-15 P2009-12-05/2009-12-21 K2011-01-28/2011-02-03", true, false},
    {"wii_locker", "Wii locker", Furniture, "Wii_locker", nullptr, "Wii_locker", pCool, "P2009-11-19/2009-12-03 K2010-03-03/2010-03-21", false, false},
    {"shopping_cart", "shopping cart", Furniture, "shopping_cart", nullptr, "shopping_cart", pRibbon, "E2009-11-27/2009-12-03", true, false},
    {"ds_lite", "Nintendo DS Lite", Furniture, "Nintendo_DS_Lite", nullptr, "Nintendo_DS_Lite", pButtercup, "E2009-12-05/2009-12-19", false, false},
    {"festive_wreath", "festive wreath", Furniture, "festive_wreath", nullptr, "festive_wreath", pElegant,
     "J2009-12-17/2009-12-25 P2009-12-24/2009-12-30 K2010-12-06/2010-12-12", true, false},
    {"snowman_head", "snowman head", Headwear, "snowman_head", nullptr, "snowman_head", pSnowy,
     "E2009-12-21/2010-01-04 E2010-12-01/2010-12-31", true, false},
    {"hagoita", "hagoita", Furniture, nullptr, nullptr, nullptr, 0, "J2010-01-01/2010-01-15", true, false},
    {"fedora_chair", "fedora chair", Furniture, "fedora_chair", nullptr, "fedora_chair", pTownView, "E2010-01-08/2010-01-22", false, false},
    {"tam_o_shanter", "tam o'shanter", Headwear, "tam_o_shanter", nullptr, "tam_o_shanter", pPlaid, "P2010-01-25/2010-02-01", true, false},
    {"cupid_bench", "Cupid bench", Furniture, "Cupid_bench", nullptr, "Cupid_bench", pPetal, "E2010-02-07/2010-02-21", true, false},
    {"pave_clock", "Pave clock", Furniture, "Pave_clock", nullptr, "Pave_clock", pPolkaDot,
     "P2010-02-08/2010-02-15 E2012-02-01/2012-02-27 J2012-02-01/2012-02-27", true, false},
    {"emperors_cap", "emperor's cap", Headwear, nullptr, nullptr, nullptr, 0, "J2010-02-14/2010-03-01", false, false},
    {"flower_bouquet", "flower bouquet", Furniture, nullptr, nullptr, nullptr, 0, "K2010-02-22/2010-02-28", true, false},
    {"blossom_lantern", "blossom lantern", Furniture, nullptr, nullptr, nullptr, 0, "J2010-03-03/2010-03-16", true, false},
    {"wildflower_floor", "wildflower floor", Flooring, "wildflower_floor", nullptr, "wildflower_floor", pPolkaDot, "E2010-03-17/2010-03-23", true, false},
    {"egg_tv", "egg TV", Furniture, "egg_TV", nullptr, "egg_TV", pRainbow,
     "P2010-03-01/2010-03-31 E2012-03-01/2012-03-20 J2012-03-01/2012-03-20", true, false},
    {"kimbap_plate", "kimbap plate", Furniture, nullptr, nullptr, nullptr, 0, "K2010-04-12/2010-04-18", false, false},
    {"gracie_dresser", "Gracie dresser", Furniture, "Gracie_dresser", nullptr, "Gracie_dresser", pTartan,
     "P2010-05-03/2010-05-16 E2012-05-01/2012-05-30 J2012-05-01/2012-05-30", false, false},
    {"kintaro_wig", "Kintaro wig + Kintaro apron", Headwear, nullptr, nullptr, nullptr, 0, "J2010-05-05/2010-05-18", true, false},
    {"white_school_cap", "white school cap", Headwear, nullptr, nullptr, nullptr, 0, "K2010-05-05/2010-05-11", true, false},
    {"school_cap", "school cap", Headwear, nullptr, nullptr, nullptr, 0, "K2010-05-05/2010-05-11", true, false},
    {"guards_helmet", "guard's helmet + guard's uniform", Headwear, "guard_s_helmet", "guard", "guard_s_helmet", pLined,
     "E2010-05-09/2010-05-15", false, false},
    {"red_horned_hat", "red horned hat + Victory Korea T-shirt", Headwear, nullptr, "victory", nullptr, 0, "K2010-06-12/2010-06-25", false, false},
    {"tsunokakushi", "tsunokakushi", Headwear, nullptr, nullptr, nullptr, 0, "J2010-06-18/2010-07-06", true, false},
    {"banana_split_hat", "banana split hat", Headwear, "banana_split_hat", nullptr, "banana_split_hat", pCloudy, "E2010-07-14/2010-07-25", true, false},
    {"samgyetang_bowl", "samgyetang bowl", Furniture, nullptr, nullptr, nullptr, 0, "K2010-07-29/2010-08-09", true, false},
    {"sweets_player", "sweets player", Furniture, "sweets_player", nullptr, "sweets_player", pLovely,
     "P2010-08-02/2010-08-15 E2012-04-01/2012-04-29 J2012-04-01/2012-04-29", false, false},
    {"shaved_ice_lamp", "shaved ice lamp", Furniture, "shaved_ice_lamp", nullptr, nullptr, pCloudy, "K2010-08-09/2010-08-15", true, false},
    {"cucumber_horse", "cucumber horse", Furniture, nullptr, nullptr, nullptr, 0, "J2010-08-15/2010-08-28", true, false},
    {"colorful_hanbok", "colorful hanbok (design)", Design, nullptr, nullptr, nullptr, 0, "K2010-09-20/2010-09-26", true, false},
    {"candy", "candy", Other, nullptr, nullptr, nullptr, 0, "E2010-09-05/2010-09-12", false, false},
    {"kappn_model", "Kapp'n model", Furniture, "Kapp_n_model", nullptr, "Kapp_n_model", pChinese, "E2010-09-15/2010-09-23", false, false},
    {"sporty_wall", "sporty wall", Wallpaper, "sporty_wall", nullptr, nullptr, pStar, "J2010-09-23/2010-10-08", true, false},
    {"hangul_tshirt", "Hangul T-shirt (design)", Design, nullptr, nullptr, nullptr, 0, "K2010-10-09/2010-10-16", true, false},
    {"sporty_floor", "sporty floor", Flooring, "sporty_floor", nullptr, nullptr, pStar, "J2010-10-10/2010-10-23", true, false},
    {"white_police_cap", "white police cap", Headwear, nullptr, nullptr, nullptr, 0, "K2010-10-18/2010-10-23", false, false},
    {"maple_umbrella", "maple umbrella", Umbrella, "maple_umbrella", nullptr, nullptr, pMapleLeaf, "K2010-10-25/2010-10-31", true, false},
    {"mushroom_rack", "mushroom rack", Furniture, "Mushroom_rack", nullptr, "Mushroom_rack", pForest,
     "P2010-10-25/2010-10-31 J2012-11-01/2012-11-29", true, false},
    {"tteok_plate", "tteok plate", Furniture, nullptr, nullptr, nullptr, 0, "K2010-11-08/2010-11-14", false, false},
    {"red_nose", "red nose", Accessory, nullptr, nullptr, nullptr, 0, "K2010-12-06/2010-12-12", true, false},
    {"snowman_vanity", "snowman vanity", Furniture, "Snowman_vanity", nullptr, "Snowman_vanity", pSnowman,
     "P2010-12-06/2010-12-12 E2012-01-01/2012-01-30 J2012-01-01/2012-01-30", true, false},
    {"jingle_tv", "Jingle TV", Furniture, "Jingle_TV", nullptr, "Jingle_TV", pElegant, "P2010-12-20/2010-12-23 J2012-12-01/2012-12-30", true, false},
    {"golden_bed", "golden bed", Furniture, "golden_bed", nullptr, "golden_bed", pSouthwest, "E2011-01-01/2011-01-30 J2011-01-01/2011-01-30 P2011-01-01/2011-01-30", false, false},
    {"golden_chair", "golden chair", Furniture, "golden_chair", nullptr, "golden_chair", pSouthwest,
     "E2011-02-01/2011-02-27 J2011-02-01/2011-02-27 P2011-02-01/2011-02-27 K2011-02-05/2011-02-27", false, false},
    {"golden_dresser", "golden dresser", Furniture, "golden_dresser", nullptr, "golden_dresser", pSouthwest, "E2011-03-01/2011-03-30 J2011-03-01/2011-03-30 P2011-03-01/2011-03-30", false, false},
    {"golden_table", "golden table", Furniture, "golden_table", nullptr, "golden_table", pSouthwest, "E2011-04-01/2011-04-29 J2011-04-01/2011-04-29 P2011-04-01/2011-04-29", false, false},
    {"golden_closet", "golden closet", Furniture, "golden_closet", nullptr, "golden_closet", pSouthwest, "E2011-05-01/2011-05-30 J2011-05-01/2011-05-30 P2011-05-01/2011-05-30", false, false},
    {"golden_wallpaper", "golden wallpaper", Wallpaper, "golden_wallpaper", nullptr, "golden_wallpaper", pSouthwest, "E2011-06-01/2011-06-29 J2011-06-01/2011-06-29 P2011-06-01/2011-06-29", false, false},
    {"golden_man", "golden man", Furniture, "golden_man", nullptr, "golden_man", pSouthwest, "E2011-07-01/2011-07-30 J2011-07-01/2011-07-30 P2011-07-01/2011-07-30", false, false},
    {"golden_woman", "golden woman", Furniture, "golden_woman", nullptr, "golden_woman", pSouthwest, "E2011-08-01/2011-08-30 J2011-08-01/2011-08-30 P2011-08-01/2011-08-30", false, false},
    {"golden_screen", "golden screen", Furniture, "golden_screen", nullptr, "golden_screen", pSouthwest, "E2011-09-01/2011-09-29 J2011-09-01/2011-09-29 P2011-09-01/2011-09-29", false, false},
    {"golden_bench", "golden bench", Furniture, "golden_bench", nullptr, "golden_bench", pSouthwest, "E2011-10-01/2011-10-30 J2011-10-01/2011-10-30 P2011-10-01/2011-10-30", false, false},
    {"golden_clock", "golden clock", Furniture, "golden_clock", nullptr, "golden_clock", pSouthwest, "E2011-11-01/2011-11-30 J2011-11-01/2011-11-29 P2011-11-01/2011-11-30", false, false},
    {"golden_carpet", "golden carpet", Flooring, "golden_carpet", nullptr, "golden_carpet", pSouthwest, "E2011-12-01/2011-12-30 J2011-12-01/2011-12-30 P2011-12-01/2011-12-30", false, false},
    {"creepy_coffin", "creepy coffin", Furniture, "creepy_coffin", nullptr, "creepy_coffin", pHalloween, "E2012-06-01/2012-06-14 J2012-06-01/2012-06-14 P2012-06-01/2012-06-14", false, false},
    {"creepy_bat_stone", "creepy bat stone", Furniture, "creepy_bat_stone", nullptr, "creepy_bat_stone", pHalloween, "E2012-06-16/2012-06-29 J2012-06-16/2012-06-29 P2012-06-16/2012-06-29", false, false},
    {"creepy_skeleton", "creepy skeleton", Furniture, "creepy_skeleton", nullptr, "creepy_skeleton", pHalloween, "E2012-07-01/2012-07-14 J2012-07-01/2012-07-14 P2012-07-01/2012-07-14", false, false},
    {"creepy_crystal", "creepy crystal", Furniture, "creepy_crystal", nullptr, "creepy_crystal", pHalloween, "E2012-07-16/2012-07-30 J2012-07-16/2012-07-30 P2012-07-16/2012-07-30", false, false},
    {"creepy_carpet", "creepy carpet", Flooring, "creepy_carpet", nullptr, "creepy_carpet", pHalloween, "E2012-08-01/2012-08-14 J2012-08-01/2012-08-14 P2012-08-01/2012-08-14", false, false},
    {"creepy_stone", "creepy stone", Furniture, "creepy_stone", nullptr, "creepy_stone", pHalloween, "E2012-08-16/2012-08-30 J2012-08-16/2012-08-30 P2012-08-16/2012-08-30", false, false},
    {"creepy_statue", "creepy statue", Furniture, "creepy_statue", nullptr, "creepy_statue", pHalloween, "E2012-09-01/2012-09-14 J2012-09-01/2012-09-14 P2012-09-01/2012-09-14", false, false},
    {"creepy_cauldron", "creepy cauldron", Furniture, "creepy_cauldron", nullptr, "creepy_cauldron", pHalloween, "E2012-09-16/2012-09-29 J2012-09-16/2012-09-29 P2012-09-16/2012-09-29", false, false},
    {"creepy_clock", "creepy clock", Furniture, "creepy_clock", nullptr, "creepy_clock", pHalloween, "E2012-10-01/2012-10-14 J2012-10-01/2012-10-14 P2012-10-01/2012-10-14", false, false},
    {"creepy_wallpaper", "creepy wallpaper", Wallpaper, "creepy_wallpaper", nullptr, "creepy_wallpaper", pHalloween, "E2012-10-16/2012-10-30 J2012-10-16/2012-10-30 P2012-10-16/2012-10-30", false, false},
    // In the archive, never distributed (found in a Nintendo channel's files); "Receive now" only.
    {"red_headgear", "red headgear", Headwear, "red_headgear", nullptr, nullptr, pStar, "", false, true},
    {"blue_pikmin", "blue Pikmin", Headwear, "blue_Pikmin_hat", nullptr, nullptr, pFlowery, "", false, true},
    {"yellow_pikmin", "yellow Pikmin", Headwear, "yellow_Pikmin_hat", nullptr, nullptr, pFlowery, "", false, true},
    {"blue_falcon", "Blue Falcon", Furniture, "Blue_Falcon", nullptr, nullptr, pCool, "", false, true},
    {"nintendo_bench", "Nintendo bench", Furniture, "Nintendo_bench", nullptr, nullptr, pCool, "", false, true},
    {"silver_roses", "silver roses", Accessory, "silver_roses", nullptr, nullptr, pElegant, "", false, true},
    {"pikmin", "Pikmin", Headwear, "Pikmin", nullptr, nullptr, pFlowery, "", false, true},
};
constexpr int kDefCount = int(sizeof(kDefs) / sizeof(kDefs[0]));

struct Window {
    char region;  // E P J K
    int y0, m0, d0, y1, m1, d1;
};
std::vector<Window> windows(const Def& d) {
    std::vector<Window> out;
    const char* p = d.dates;
    while (p && *p) {
        while (*p == ' ') p++;
        Window w{};
        if (std::sscanf(p, "%c%d-%d-%d/%d-%d-%d", &w.region, &w.y0, &w.m0, &w.d0, &w.y1, &w.m1, &w.d1) == 7) out.push_back(w);
        while (*p && *p != ' ') p++;
    }
    return out;
}

// ---- running game: region and language -------------------------------------------------------

char gameRegion() {
    switch (port::install().running) {
    case port::GameVersion::PAL: return 'P';
    case port::GameVersion::JPN: return 'J';
    case port::GameVersion::KOR: return 'K';
    default: return 'E';
    }
}
// item.bin name field of the game's language (BITM offsets; 0x22 bytes UTF-16).
u32 itemNameOffset() {
    const int l = sys::effectiveLanguage();
    switch (gameRegion()) {
    case 'J': return 0x012;
    case 'K': return 0x144;
    case 'P': return l == sys::German ? 0x0BC : l == sys::Italian ? 0x0DE : l == sys::Spanish ? 0x100 : l == sys::French ? 0x122 : 0x09A;
    default: return l == sys::Spanish ? 0x056 : l == sys::French ? 0x078 : 0x034;
    }
}
// Letter files of a package for the game's region (ltr<region><language>.bmg) and the header line.
struct LetterLang {
    const char* code;
    const char* header;  // \n = where the game puts the player's name
};
std::vector<LetterLang> letterLangs() {
    switch (gameRegion()) {
    case 'P': return {{"ee", "Dear \n,"}, {"ef", "\xC3\x80 \n,"}, {"eg", "Hallo \n!"}, {"ei", "Ciao \n,"}, {"es", "\xC2\xA1Hola, \n!"}};
    case 'J': return {{"jj", "Dear \n,"}};
    case 'K': return {{"kk", "Dear \n,"}};
    default: return {{"ue", "Dear \n,"}, {"uf", "\xC3\x80 \n,"}, {"us", "\xC2\xA1Hola, \n!"}};
    }
}

// ---- local files: archive cache, state ----------------------------------------------------------

fs::path folder() { return port::dataDir() / "distributions"; }
fs::path cacheFolder() { return folder() / "wiilink-archive"; }

std::vector<u8> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
std::string sha1Hex(const std::vector<u8>& d) {
    u8 h[20];
    wc24crypto::sha1(d.data(), d.size(), h);
    char s[41];
    for (int i = 0; i < 20; i++) std::snprintf(s + 2 * i, 3, "%02x", h[i]);
    return s;
}
const ArchiveFile* archiveEntry(const std::string& path) {
    for (const auto& a : kArchive)
        if (path == a.path) return &a;
    return nullptr;
}

std::mutex sM;
std::map<std::string, std::vector<u8>> sFiles;  // verified archive files by path
std::atomic<bool> sFetching{false};
std::string sFetchStatus = "not downloaded yet";
int sFetchOk = 0, sFetchFail = 0;

// Loads a cached archive file if its hash matches.
bool loadCached(const ArchiveFile& a) {
    auto d = readFile(cacheFolder() / a.path);
    if (d.size() != a.size || sha1Hex(d) != a.sha1) return false;
    sFiles[a.path] = std::move(d);
    return true;
}

void fetchArchive() {
    if (sFetching.exchange(true)) return;
    std::thread([] {
        int ok = 0, fail = 0, fetched = 0;
        std::string lastError;
        for (const auto& a : kArchive) {
            {
                std::lock_guard lk(sM);
                if (sFiles.count(a.path) || loadCached(a)) {
                    ok++;
                    continue;
                }
            }
            if (!enabled()) break;  // the download only happens while the feature is on
            const auto res = port::http::get(std::string(kArchiveBase) + a.path, 15000, 64 * 1024);
            struct {
                int status;
                std::string error;
                std::vector<u8> body;
            } r{res.status, res.error, std::vector<u8>(res.body.begin(), res.body.end())};
            if (r.status == 200 && r.error.empty() && r.body.size() == a.size && sha1Hex(r.body) == a.sha1) {
                std::error_code ec;
                const fs::path p = cacheFolder() / a.path;
                fs::create_directories(p.parent_path(), ec);
                std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(r.body.data()), std::streamsize(r.body.size()));
                std::lock_guard lk(sM);
                sFiles[a.path] = r.body;
                ok++, fetched++;
            } else {
                fail++;
                lastError = std::string(a.path) + ": " +
                            (!r.error.empty() ? r.error : r.status != 200 ? "HTTP " + std::to_string(r.status) : "content does not match the pinned hash");
            }
        }
        {
            std::lock_guard lk(sM);
            sFetchOk = ok, sFetchFail = fail;
            char b[160];
            std::snprintf(b, sizeof(b), "%d of %d archive files ready (%d downloaded now)", ok, int(std::size(kArchive)), fetched);
            sFetchStatus = b;
            if (fail) sFetchStatus += "; failed: " + lastError;
        }
        recomp::log("[distrib] archive: %s", sFetchStatus.c_str());
        sFetching = false;
        nwc24::requestDownload();
    }).detach();
}

const std::vector<u8>* file(const std::string& path) {
    auto it = sFiles.find(path);
    return it == sFiles.end() ? nullptr : &it->second;
}
std::string itemPath(const Def& d) { return d.item ? std::string("items/") + d.item + ".bin" : std::string(); }
std::string designPath(const Def& d) { return d.design ? std::string("designs/") + d.design + ".bin" : std::string(); }
std::string letterPath(const Def& d) { return d.letter ? std::string("stuff/") + d.letter + ".txt" : std::string(); }
bool inArchive(const Def& d) { return d.item || d.design; }
bool archiveReady(const Def& d) {
    return (!d.item || file(itemPath(d))) && (!d.design || file(designPath(d)));
}

// Per-town record (state.ini in the distributions folder, distrib_record.h): which distributions
// each town got and when (and which through "Receive now", once per town), the last letter id
// served, a counter for "Receive now" letters. Towns are keyed by their own id + name from the save.
using record::TownState;
struct State : record::Book {
    int pending = -1;      // "Receive now": catalogue index waiting for KD
    u32 pendingId = 0;
    std::string pendingLocal;  // "Receive now" of an original .enc from the folder
    bool syncWanted = false;  // a package was stored: run the game's check while playing
};
State sState;
bool sStateLoaded = false;
std::atomic<bool> sTownSeen{false};  // the current town was identified from the save in this session
int sNowWaiting = -1;  // "Receive now" package KD stored that the game has not kept yet (this session)
std::map<std::string, std::string> sLocalResult;  // original file -> KD's verdict
u32 sPeteDelivered = 0;  // letter id Pete last handed over (this session)

void saveState() {
    std::error_code ec;
    fs::create_directories(folder(), ec);
    const fs::path p = folder() / "state.ini", tmp = folder() / "state.ini.tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        record::write(sState, f);
        if (!f) return;
    }
    fs::rename(tmp, p, ec);  // whole file or nothing: the record must not get lost on a crash
    if (ec) {
        fs::remove(p, ec);
        fs::rename(tmp, p, ec);
    }
}
void loadState() {
    if (sStateLoaded) return;
    sStateLoaded = true;
    {
        std::ifstream f(folder() / "state.ini");
        if (!f) {
            sState.format = record::kFormat;  // a new record
            return;
        }
        record::parse(sState, f);
    }
    // Files from before the "Receive now once" change: what "Receive now" delivered then counts.
    const bool old = sState.format < record::kFormat;
    if (record::upgrade(sState, [](int di) { return di >= 0 && di < kDefCount ? kDefs[di].key : nullptr; })) {
        int n = 0;
        for (const auto& [k, t] : sState.towns) n += int(t.now.size());
        if (old) recomp::log("[distrib] record from an older version: %d distribution(s) counted as received through Receive now", n);
        saveState();
    }
}
TownState& town() { return sState.towns[sState.town]; }

// "Receive now" once per town: the time this town kept `key` through it (0 = never).
long long receivedNowAt(const std::string& key) {
    const auto& n = town().now;
    const auto it = n.find(key);
    return it == n.end() ? 0 : std::max(1LL, it->second);
}
std::string encKey(const std::vector<u8>& data) {
    u8 h[20];
    wc24crypto::sha1(data.data(), data.size(), h);
    std::string k = "enc:";
    char b[4];
    for (u8 x : h) std::snprintf(b, sizeof b, "%02x", x), k += b;
    return k;
}

// The town in the save (town_id.h: town id + name; shared with online.cpp's town_wc24.txt).
// "" while no town is loaded.
std::string townInSave(std::string* readable, u32 save = 0) {
    // In an online room the town data may be another player's town (a visit): keep the last one.
    if (recomp::online::active()) return "";
    return recomp::town::keyInSave(readable, save);
}
// Called with sM held: the game shows this town (its record becomes the current one).
void noteTown(const std::string& key, const std::string& readable) {
    if (key.empty()) return;
    loadState();
    const bool first = !sTownSeen || sState.town != key;
    sTownSeen = true;
    if (record::identify(sState, key, readable)) saveState();
    if (first) {
        std::string got;
        for (const auto& [k, when] : town().now) got += (got.empty() ? "" : ", ") + k;
        recomp::log("[distrib] town %s (%s): received through Receive now: %s", readable.c_str(), key.c_str(),
                    got.empty() ? "none" : got.c_str());
        nwc24::requestDownload();  // KD's check waits for the town (serve)
    }
}
std::string dateText(long long secs2000);

// ---- original .enc files the player supplies ----------------------------------------------------
struct LocalEnc {
    fs::path path;
    std::vector<u8> data;
};
std::vector<LocalEnc> localFiles() {
    std::vector<LocalEnc> out;
    std::error_code ec;
    for (auto it = fs::directory_iterator(folder(), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string ext = it->path().extension().string();
        for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (ext != ".enc" && ext != ".wc24") continue;
        auto d = readFile(it->path());
        if (d.size() >= 0x140 && d.size() < (4u << 20) && std::memcmp(d.data(), "WC24", 4) == 0) out.push_back({it->path(), std::move(d)});
    }
    std::sort(out.begin(), out.end(), [](const LocalEnc& a, const LocalEnc& b) { return a.path < b.path; });
    return out;
}

// ---- calendar ------------------------------------------------------------------------------------
struct Date {
    int y, m, d;
};
long long daysFromCivil(int y, int m, int d) {  // days since 1970-01-01 (proleptic Gregorian)
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}
Date civil(long long z) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = unsigned(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const int d = int(doy - (153 * mp + 2) / 5 + 1);
    const int m = int(mp < 10 ? mp + 3 : mp - 9);
    return {int(yoe + era * 400) + (m <= 2), m, d};
}
constexpr long long kDay = 86400;
const long long kEpoch2000 = daysFromCivil(2000, 1, 1);
// Console clock (local time, seconds since 2000-01-01) -> day number since 1970.
long long consoleDay(long long secs2000) { return kEpoch2000 + secs2000 / kDay; }
long long consoleNow() { return (long long)(double(timeBase()) / kTimerClock); }
std::string dateText(long long secs2000) {  // the game clock's date, YYYY-MM-DD
    const Date d = civil(consoleDay(secs2000));
    char b[16];
    std::snprintf(b, sizeof b, "%04d-%02d-%02d", d.y, d.m, d.d);
    return b;
}

// ---- which distributions are in play --------------------------------------------------------------

bool nameInLanguage(const Def& d) {
    if (!d.item) return true;  // designs carry every language
    const auto* f = file(itemPath(d));
    if (!f) return true;
    const u32 o = itemNameOffset();
    return f->size() >= o + 2 && ((*f)[o] || (*f)[o + 1]);
}
bool distributedIn(const Def& d, char region) {
    for (const auto& w : windows(d))
        if (w.region == region) return true;
    return false;
}
// In the loop / original schedules: released, in the archive, distributed in the game's region
// (or another one when the option is on and its item has a name in the game's language).
bool inPlay(const Def& d) {
    if (d.unreleased || !inArchive(d)) return false;
    if (distributedIn(d, gameRegion())) return true;
    return port::settings().events.otherRegions && nameInLanguage(d);
}
// Its month: the first window in the game's region, else the first window.
int originalMonth(const Def& d) {
    const auto ws = windows(d);
    for (const auto& w : ws)
        if (w.region == gameRegion()) return w.m0;
    return ws.empty() ? 1 : ws[0].m0;
}
u32 fnv(const char* s) {
    u32 h = 2166136261u;
    for (; *s; s++) h = (h ^ u8(*s)) * 16777619u;
    return h;
}

// ---- the loop schedule ---------------------------------------------------------------------------
// Slots: every 7 or 14 days from Monday 2000-01-03, or the 1st of every month. Each cycle hands
// out every distribution in play once; a seasonal one goes into a slot of its original month
// when one is free, the others fill the remaining slots in a fixed spread order. The same for
// everyone (a function of the catalogue and the date only).
struct Slot {
    long long index = -1;
    long long startDay = 0;  // day number of the slot's first day
    int dist = -1;
    int cycle = 0;
};
long long slotIndexAt(long long day) {
    const int p = port::settings().events.loopPeriod;
    if (p == 2) {
        const Date dt = civil(day);
        return (long long)(dt.y - 2000) * 12 + (dt.m - 1);
    }
    const long long anchor = daysFromCivil(2000, 1, 3);
    return day < anchor ? 0 : (day - anchor) / (p == 1 ? 14 : 7);
}
long long slotStart(long long s) {
    const int p = port::settings().events.loopPeriod;
    if (p == 2) return daysFromCivil(2000 + int(s / 12), int(s % 12) + 1, 1);
    return daysFromCivil(2000, 1, 3) + s * (p == 1 ? 14 : 7);
}

std::vector<Slot> sLoop;  // cache: slots 0..n
std::string sLoopKey;
void buildLoop(long long upTo) {
    std::vector<int> all;
    for (int i = 0; i < kDefCount; i++)
        if (inPlay(kDefs[i])) all.push_back(i);
    std::string key = std::to_string(port::settings().events.loopPeriod) + (port::settings().events.otherRegions ? "o" : "") +
                      gameRegion() + std::to_string(all.size());
    if (key != sLoopKey) sLoop.clear(), sLoopKey = key;
    if (all.empty()) return;
    std::set<int> remaining;
    int cycle = 0;
    if (!sLoop.empty()) {  // resume: recompute the remaining set of the last cycle
        cycle = sLoop.back().cycle;
        remaining.insert(all.begin(), all.end());
        for (auto it = sLoop.rbegin(); it != sLoop.rend() && it->cycle == cycle; ++it) remaining.erase(it->dist);
    }
    // Spread order of the non-seasonal ones: by hash of the key (fixed, mixes the series).
    auto spread = [](int a, int b) { return fnv(kDefs[a].key) < fnv(kDefs[b].key); };
    for (long long s = (long long)sLoop.size(); s <= upTo; s++) {
        if (remaining.empty()) {
            remaining.insert(all.begin(), all.end());
            if (s) cycle++;
        }
        const int month = civil(slotStart(s)).m;
        int pick = -1;
        for (int i : remaining)  // seasonal, in its month: the earliest original date first
            if (kDefs[i].seasonal && originalMonth(kDefs[i]) == month && (pick < 0 || spread(i, pick))) pick = i;
        if (pick < 0)
            for (int i : remaining)
                if (!kDefs[i].seasonal && (pick < 0 || spread(i, pick))) pick = i;
        if (pick < 0) {  // only seasonal ones left, none for this month: the one whose month comes next
            int best = 99;
            for (int i : remaining) {
                const int ahead = (originalMonth(kDefs[i]) - month + 12) % 12;
                if (ahead < best || (ahead == best && spread(i, pick))) best = ahead, pick = i;
            }
        }
        remaining.erase(pick);
        sLoop.push_back({s, slotStart(s), pick, cycle});
    }
}
Slot loopSlot(long long day) {
    const long long s = slotIndexAt(day);
    if (s > 200000) return {};
    buildLoop(s);
    return s < (long long)sLoop.size() ? sLoop[size_t(s)] : Slot{};
}

// ---- the original-dates schedule -------------------------------------------------------------------
// Windows of the game's region (and others when the option is on) containing today: every year
// on the same month/day (default), or only in the original year.
struct Due {
    int dist = -1;
    int year = 0;  // the year of the window instance (for the letter id)
};
std::vector<Due> originalDue(long long day) {
    const Date today = civil(day);
    std::vector<std::pair<long long, Due>> c;
    for (int i = 0; i < kDefCount; i++) {
        const Def& d = kDefs[i];
        if (!inPlay(d)) continue;
        for (const auto& w : windows(d)) {
            if (w.region != gameRegion() && !(port::settings().events.otherRegions && nameInLanguage(d))) continue;
            if (port::settings().events.originalYearly) {
                for (int y : {today.y - 1, today.y}) {  // a window may run over New Year
                    const long long a = daysFromCivil(y, w.m0, w.d0);
                    const long long b = daysFromCivil(y + (w.y1 - w.y0), w.m1, w.d1);
                    if (day >= a && day <= b) c.push_back({a, {i, y}});
                }
            } else {
                const long long a = daysFromCivil(w.y0, w.m0, w.d0), b = daysFromCivil(w.y1, w.m1, w.d1);
                if (day >= a && day <= b) c.push_back({a, {i, w.y0}});
            }
        }
    }
    std::sort(c.begin(), c.end(), [](auto& x, auto& y) { return x.first < y.first; });
    std::vector<Due> out;
    for (auto& [a, due] : c) out.push_back(due);
    return out;
}

// ---- packages ------------------------------------------------------------------------------------

void putBe32(std::vector<u8>& v, size_t o, u32 x) {
    v[o] = u8(x >> 24), v[o + 1] = u8(x >> 16), v[o + 2] = u8(x >> 8), v[o + 3] = u8(x);
}
std::vector<u8> utf16be(const std::string& s) {
    std::vector<u8> out;
    for (size_t i = 0; i < s.size();) {
        u32 c = u8(s[i]);
        int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (n == 2) c &= 0x1F;
        else if (n == 3) c &= 0x0F;
        else if (n == 4) c &= 0x07;
        for (int k = 1; k < n && i + k < s.size(); k++) c = (c << 6) | (u8(s[i + k]) & 0x3F);
        i += size_t(n);
        if (c > 0xFFFF) c = '?';
        out.push_back(u8(c >> 8));
        out.push_back(u8(c));
    }
    return out;
}

// A letter as the game's BMG (MESGbmg1, UTF-16): 7 messages = (empty), header, body, footer,
// sender, paper number, (empty); the same layout and attributes as Nintendo's letters.
std::vector<u8> bmgLetter(const std::string& header, const std::string& body, const std::string& footer,
                          const std::string& sender, int paper) {
    const std::string texts[7] = {"", header, body, footer, sender, std::to_string(400 + paper), ""};
    std::vector<u8> dat = {0, 0};  // offset 0 is not used
    u32 offs[7];
    for (int i = 0; i < 7; i++) {
        offs[i] = u32(dat.size());
        auto t = utf16be(texts[i]);
        dat.insert(dat.end(), t.begin(), t.end());
        dat.push_back(0), dat.push_back(0);
    }
    const size_t datSize = (8 + dat.size() + 31) / 32 * 32;
    const size_t infSize = 0xA0, midSize = 0x40;
    std::vector<u8> b(0x20 + infSize + datSize + midSize, 0);
    std::memcpy(&b[0], "MESGbmg1", 8);
    putBe32(b, 8, u32(b.size()));
    putBe32(b, 12, 3);
    b[16] = 2;  // UTF-16
    size_t o = 0x20;
    std::memcpy(&b[o], "INF1", 4);
    putBe32(b, o + 4, u32(infSize));
    b[o + 8] = 0, b[o + 9] = 7, b[o + 10] = 0, b[o + 11] = 0x14;
    for (int i = 0; i < 7; i++) {
        const size_t e = o + 16 + 0x14 * size_t(i);
        putBe32(b, e, offs[i]);
        if (i == 1) b[e + 7] = 2, b[e + 8] = 0x14;  // the header line's attributes
        else b[e + 19] = 1;
    }
    o += infSize;
    std::memcpy(&b[o], "DAT1", 4);
    putBe32(b, o + 4, u32(datSize));
    std::memcpy(&b[o + 8], dat.data(), dat.size());
    o += datSize;
    std::memcpy(&b[o], "MID1", 4);
    putBe32(b, o + 4, u32(midSize));
    b[o + 8] = 0, b[o + 9] = 7, b[o + 10] = 0x10;
    for (int i = 0; i < 7; i++) putBe32(b, o + 16 + 4 * size_t(i), u32(i));
    return b;
}

// U8 archive (the game's ARC reader): root directory + files, data 32-byte aligned.
std::vector<u8> u8Archive(const std::vector<std::pair<std::string, std::vector<u8>>>& files) {
    std::vector<u8> names = {0};
    std::vector<u32> nameOff;
    for (auto& f : files) {
        nameOff.push_back(u32(names.size()));
        names.insert(names.end(), f.first.begin(), f.first.end());
        names.push_back(0);
    }
    const u32 nodes = u32(files.size() + 1);
    const u32 hdrSize = nodes * 12 + u32(names.size());
    const u32 dataStart = (0x20 + hdrSize + 31) / 32 * 32;
    u32 total = dataStart;
    std::vector<u32> dataOff;
    for (auto& f : files) {
        dataOff.push_back(total);
        total = (total + u32(f.second.size()) + 31) / 32 * 32;
    }
    std::vector<u8> a(total, 0);
    putBe32(a, 0, 0x55AA382D);
    putBe32(a, 4, 0x20);
    putBe32(a, 8, hdrSize);
    putBe32(a, 12, dataStart);
    size_t n = 0x20;
    a[n] = 1;  // root directory
    putBe32(a, n + 8, nodes);
    for (size_t i = 0; i < files.size(); i++) {
        const size_t e = 0x20 + 12 * (i + 1);
        a[e + 1] = u8(nameOff[i] >> 16), a[e + 2] = u8(nameOff[i] >> 8), a[e + 3] = u8(nameOff[i]);
        putBe32(a, e + 4, dataOff[i]);
        putBe32(a, e + 8, u32(files[i].second.size()));
        std::memcpy(&a[dataOff[i]], files[i].second.data(), files[i].second.size());
    }
    std::memcpy(&a[0x20 + 12 * nodes], names.data(), names.size());
    return a;
}

std::string itemName(const Def& d) {
    if (d.item)
        if (const auto* f = file(itemPath(d))) {
            const u32 o = itemNameOffset();
            std::string s;
            for (u32 i = 0; i + 1 < 0x22 && o + i + 1 < f->size(); i += 2) {
                const u32 c = u32((*f)[o + i]) << 8 | (*f)[o + i + 1];
                if (!c) break;
                if (c < 0x80) s += char(c);
                else if (c < 0x800) s += char(0xC0 | (c >> 6)), s += char(0x80 | (c & 0x3F));
                else s += char(0xE0 | (c >> 12)), s += char(0x80 | ((c >> 6) & 0x3F)), s += char(0x80 | (c & 0x3F));
            }
            if (!s.empty()) return s;
        }
    return d.name;
}

// The archive letter (English text): first line header, last line footer, the rest the body.
bool archiveLetter(const Def& d, std::string& body, std::string& footer) {
    if (!d.letter) return false;
    const auto* f = file(letterPath(d));
    if (!f || f->empty()) return false;
    std::vector<std::string> lines;
    std::string cur;
    for (u8 ch : *f) {
        if (ch == '\r') continue;
        if (ch == '\n') lines.push_back(cur), cur.clear();
        else cur += char(ch);
    }
    if (!cur.empty()) lines.push_back(cur);
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    if (lines.size() < 3) return false;
    footer = lines.back();
    std::string b;
    for (size_t i = 1; i + 1 < lines.size(); i++) {
        if (lines[i].empty()) continue;
        if (!b.empty()) b += "\n";
        b += lines[i];
    }
    body = b;
    return !body.empty();
}

std::vector<u8> buildPackage(int di, u32 letterId) {
    const Def& d = kDefs[di];
    std::vector<std::pair<std::string, std::vector<u8>>> files;
    std::vector<u8> info(0x14, 0);
    putBe32(info, 0x00, 1);
    putBe32(info, 0x04, 1);
    putBe32(info, 0x08, d.item ? letterId : 0);   // letter + item (Pete); 0 = none
    putBe32(info, 0x0C, d.design ? letterId : 0); // design (Wendell); 0 = none
    if (d.design) files.push_back({"design.bin", *file(designPath(d))});
    files.push_back({"info.bin", info});
    if (d.item) {
        files.push_back({"item.bin", *file(itemPath(d))});
        std::string body, footer;
        if (!archiveLetter(d, body, footer)) {
            body = "Here is a little gift\nfrom all of us:\n" + itemName(d) + ".\nWe hope you enjoy it!";
            footer = "Nintendo";
        }
        for (const auto& l : letterLangs()) files.push_back({std::string("ltr") + l.code + ".bmg", bmgLetter(l.header, body, footer, "Nintendo", d.paper)});
    }
    std::sort(files.begin(), files.end(), [](auto& a, auto& b) { return a.first < b.first; });
    return u8Archive(files);
}

bool forceEnv(int& v) {
    const char* e = std::getenv("CF_DISTRIB");
    if (!e || !*e) return false;
    if (!std::strcmp(e, "off")) v = 0;
    else if (!std::strcmp(e, "loop")) v = 1;
    else if (!std::strcmp(e, "original")) v = 2;
    else if (!std::strcmp(e, "manual")) v = 3;
    else v = std::atoi(e);
    return true;
}

// Test aid CF_DISTRIB_NOW=<key>[@<seconds>]: "Receive now" for that distribution once the game
// has registered its task and the town is known (after <seconds> of play, default at once).
// Several, comma-separated, are pressed in order (e.g. the same key twice: the second is refused).
int sTestNow = -2;
double sTestNowAt = 0;
std::vector<std::pair<int, double>> sTestNowList;  // the ones after the first

int indexOf(const std::string& key) {
    for (int i = 0; i < kDefCount; i++)
        if (key == kDefs[i].key) return i;
    return -1;
}

// "Receive now": at most once per town (anti-farming; the schedules still bring every
// distribution again in its slot). Refused for a distribution this town already kept through it.
bool receiveNow(int di) {
    loadState();
    if (const long long at = receivedNowAt(kDefs[di].key)) {
        recomp::log("[distrib] receive now refused: %s was received through Receive now on %s (town %s)", kDefs[di].key,
                    dateText(at).c_str(), sState.town.c_str());
        return false;
    }
    if (sState.pending == di || sNowWaiting == di) {
        recomp::log("[distrib] receive now ignored: %s is already on its way", kDefs[di].key);
        return false;
    }
    sState.pending = di;
    sState.pendingId = 0x4D000000u | (++sState.manualCounter & 0xFFFFFF);  // 'M'
    saveState();
    recomp::log("[distrib] receive now: %s (letter id %08X)", kDefs[di].key, sState.pendingId);
    nwc24::requestDownload();
    return true;
}

// The game kept a "Receive now" letter (fn_8010F194; an original file: KD stored the package).
void noteReceivedNow(const std::string& key) {
    sNowWaiting = -1;
    TownState& t = town();
    if (t.now.count(key)) return;
    t.now[key] = consoleNow();
    recomp::log("[distrib] %s: received through Receive now by town %s (no second time for this town)", key.c_str(),
                sState.town.c_str());
}

#ifdef CF_DEV_TOOLS
// Developer builds only (cfrecomp; never in the shipped CityFolkRecomp): forget this town's
// "Receive now" record, all of it (key empty) or one distribution.
void forgetReceivedNow(const std::string& key) {
    loadState();
    auto& n = town().now;
    const size_t before = n.size();
    if (key.empty()) n.clear();
    else n.erase(key);
    saveState();
    recomp::log("[distrib] dev: forgot %zu Receive now record(s) of town %s", before - n.size(), sState.town.c_str());
}
#endif

}  // namespace

Mode mode() {
    int v = port::settings().events.mode;
    forceEnv(v);
    return Mode(std::clamp(v, 0, 3));
}
bool enabled() { return mode() != Mode::Off; }
int count() { return kDefCount; }

Reply serve(const std::string& url) {
    Reply r;
    std::lock_guard lk(sM);
    loadState();
    if (url.find("/ruu/rvforestdl_") == std::string::npos) {
        r.what = "not a Nintendo event URL (" + url + ")";
        return r;
    }
    if (sFiles.size() < std::size(kArchive) && !sFetching) fetchArchive();
    // Original files the player supplied: served first (each once, in file order), as they are.
    // (Which distribution one carries is known only after KD decrypts it.)
    // "Receive now" wins over the schedule.
    // Whose record decides: the town in the save. Until one is loaded in this session (KD checks
    // at boot, before the save is read), nothing is served: the last town's record is not
    // necessarily this one's (another save in the same data folder).
    {
        std::string readable;
        const std::string k = townInSave(&readable);
        if (!k.empty()) noteTown(k, readable);
        else if (!sTownSeen) {
            r.kind = Reply::NotModified;
            r.what = "waiting for the town to be loaded";
            return r;
        }
    }
    int di = -1;
    u32 id = 0;
    std::string why;
    // "Receive now" once per town, checked again here (a request from before the town was known).
    if (sState.pending >= 0 && receivedNowAt(kDefs[sState.pending].key)) {
        recomp::log("[distrib] receive now dropped: %s was already received through Receive now by this town",
                    kDefs[sState.pending].key);
        sState.pending = -1;
    }
    if (!sState.pendingLocal.empty() && receivedNowAt(encKey(readFile(sState.pendingLocal)))) {
        recomp::log("[distrib] receive now dropped: this town already received %s", sState.pendingLocal.c_str());
        sState.pendingLocal.clear();
    }
    if (!sState.pendingLocal.empty()) {
        r.body = readFile(sState.pendingLocal);
        r.kind = r.body.empty() ? Reply::NotFound : Reply::Container;
        r.what = "original file " + port::displayPath(fs::path(sState.pendingLocal).filename());
        r.localFile = sState.pendingLocal;
        return r;
    }
    if (sState.pending >= 0) {
        di = sState.pending, id = sState.pendingId, why = "receive now";
    } else {
        const long long day = consoleDay(consoleNow());
        TownState& t = town();
        if (mode() == Mode::Loop) {
            const Slot s = loopSlot(day);
            if (s.dist >= 0) {
                auto c = t.cycle.find(kDefs[s.dist].key);
                const bool had = c != t.cycle.end() && c->second >= s.cycle && t.received.count(kDefs[s.dist].key);
                // One delivery per slot: one came in this slot already (e.g. "Receive now"): the
                // slot's own waits for the next one (the game keeps one Nintendo letter at a time).
                const bool slotUsed = t.lastDelivery && consoleDay(t.lastDelivery) >= s.startDay;
                if (slotUsed && !had) {
                    r.kind = Reply::NotModified;
                    r.what = "a letter came in this slot already; next slot";
                } else if (!had) di = s.dist, id = 0x4C000000u | u32(s.index & 0xFFFFFF), why = "loop slot " + std::to_string(s.index);
                else r.kind = Reply::NotModified, r.what = std::string(kDefs[s.dist].key) + " already received this cycle";
            }
        } else if (mode() == Mode::Original) {
            for (const Due& d : originalDue(day)) {
                auto it = t.received.find(kDefs[d.dist].key);
                const bool had = it != t.received.end() && civil(consoleDay(it->second)).y >= d.year &&
                                 consoleDay(it->second) >= day - 400;
                if (had) continue;
                if (t.lastDelivery && consoleDay(t.lastDelivery) > day - 7) {  // one per week at most
                    r.kind = Reply::NotModified, r.what = "next one after a week";
                    break;
                }
                di = d.dist, id = 0x4F000000u | u32(d.dist) << 12 | u32(d.year & 0xFFF), why = "original date";
                break;
            }
        }
        if (di < 0 && r.kind == Reply::NotFound) {
            r.what = mode() == Mode::Manual ? "only when you choose" : "nothing due today";
            return r;
        }
        if (di < 0) return r;
    }
    if (id == sState.lastStored) {
        r.kind = Reply::NotModified;
        r.what = std::string(kDefs[di].key) + " (already in wc24dl.vff)";
        return r;
    }
    if (!archiveReady(kDefs[di])) {
        r.kind = Reply::NotFound;
        r.what = std::string(kDefs[di].key) + ": archive files not downloaded yet";
        return r;
    }
    r.kind = Reply::Package;
    r.dist = di;
    r.letterId = id;
    r.body = buildPackage(di, id);
    r.what = std::string(kDefs[di].name) + " (" + why + ", letter " + [&] {
        char b[12];
        std::snprintf(b, sizeof(b), "%08X", id);
        return std::string(b);
    }() + ")";
    sState.served[id] = di;
    return r;
}

void onStored(const Reply& r, bool ok, const std::string& why) {
    std::lock_guard lk(sM);
    if (!ok) {
        recomp::log("[distrib] not stored: %s", why.c_str());
        if (!r.localFile.empty() && r.localFile == sState.pendingLocal) sState.pendingLocal.clear();  // KD refused it
        if (!r.localFile.empty()) sLocalResult[r.localFile] = why;
        return;
    }
    if (!r.localFile.empty()) {
        sLocalResult[r.localFile] = "stored";
        // An original file through "Receive now": once per town too (by the file's SHA-1; which
        // distribution it carries is not known here). Counted when KD stored it.
        if (r.localFile == sState.pendingLocal) noteReceivedNow(encKey(r.body));
    }
    if (sState.pending >= 0 && r.letterId == sState.pendingId) sState.pending = -1;
    // wc24dl.vff holds one package: a "Receive now" one waits there until the game keeps it.
    sNowWaiting = record::isReceiveNowId(r.letterId) ? r.dist : -1;
    if (r.letterId) sState.lastStored = r.letterId;
    if (!r.localFile.empty() && r.localFile == sState.pendingLocal) sState.pendingLocal.clear();
    sState.syncWanted = true;
    saveState();
}

// The game keeps a Nintendo letter (fn_8010F194, hooked on every version: r3 = the save's letter slot, r4 = letter
// text, r5 = item, r6 = letter id): Pete will bring it. Recorded for the town.
HLE(fn_8010F194) {
    const u32 id = c.r[6], slot = c.r[3];
    callOriginal(c, "fn_8010F194");
    std::lock_guard lk(sM);
    loadState();
    auto it = sState.served.find(id);
    recomp::log("[distrib] the game took Nintendo letter %08X (%s): Pete brings it at the next town load", id,
                it != sState.served.end() ? kDefs[it->second].key : "not from this port");
    if (it == sState.served.end()) return;
    {  // the town whose save keeps the letter (the job runs while the save is loaded, before the
       // game's town data pointer is set at boot): its data = the letter slot - 0xFC980
        std::string readable;
        std::string k = townInSave(&readable, slot - 0xFC980u);
        if (k.empty()) k = townInSave(&readable);
        noteTown(k, readable);
    }
    TownState& t = town();
    t.received[kDefs[it->second].key] = consoleNow();
    t.cycle[kDefs[it->second].key] = loopSlot(consoleDay(consoleNow())).cycle;
    t.lastDelivery = consoleNow();
    if (record::isReceiveNowId(id)) noteReceivedNow(kDefs[it->second].key);
    saveState();
}

// Pete (d_a_npc_sp_perioNP) hands the kept Nintendo letter to the player (fn_80104520:
// copies the save's Nintendo-letter slot into the player's mail; r3 = 1 when it did).
HLE(fn_80104520) {
    const u32 save = rd32(VA(0x8074E960));
    const u32 id = save ? rd32(save + 0xFC980u) : 0;
    callOriginal(c, "fn_80104520");
    recomp::log("[distrib] Pete handed Nintendo letter %08X to the player (result %u)", id, c.r[3]);
    std::lock_guard lk(sM);
    sPeteDelivered = c.r[3] ? id : sPeteDelivered;
}

void onGuestFrame(Ctx& c) {
    if (!enabled()) return;
    if (sTestNow == -2) {
        sTestNow = -1;
        // CF_DISTRIB_DESCRIBE=<n>: the catalogue per region and the next n loop slots (docs).
        if (const char* e = std::getenv("CF_DISTRIB_DESCRIBE")) {
            for (char reg : {'E', 'P', 'J', 'K'}) {
                int total = 0, archived = 0;
                std::string missing;
                for (const Def& d : kDefs) {
                    if (d.unreleased || !distributedIn(d, reg)) continue;
                    total++;
                    if (inArchive(d)) archived++;
                    else missing += std::string(missing.empty() ? "" : ", ") + d.name;
                }
                recomp::log("[distrib] region %c: %d distributions, %d in the archive; missing: %s", reg, total, archived,
                            missing.c_str());
            }
            std::istringstream lines(describeSchedule(consoleNow(), std::max(1, std::atoi(e))));
            for (std::string l; std::getline(lines, l);) recomp::log("[distrib] slot %s", l.c_str());
        }
        if (const char* e = std::getenv("CF_DISTRIB_NOW")) {
            std::istringstream items(e);
            for (std::string v; std::getline(items, v, ',');) {
                double when = 0;
                const auto at = v.find('@');
                if (at != std::string::npos) when = std::atof(v.c_str() + at + 1), v.resize(at);
                const int di = indexOf(v);
                if (di < 0) recomp::log("[distrib] CF_DISTRIB_NOW: unknown key %s", v.c_str());
                else sTestNowList.push_back({di, when});
            }
            if (!sTestNowList.empty()) {
                sTestNow = sTestNowList[0].first, sTestNowAt = sTestNowList[0].second;
                sTestNowList.erase(sTestNowList.begin());
            }
        }
    }
    // Which town is loaded (every version): its record is the one "Receive now" checks.
    static u32 sIdFrame = 0;
    if (++sIdFrame % 60 == 0) {
        std::string readable;
        const std::string k = townInSave(&readable);
        if (!k.empty()) {
            std::lock_guard lk(sM);
            noteTown(k, readable);
#ifdef CF_DEV_TOOLS
            // Test aid (developer builds only) CF_DISTRIB_FORGET=1|<key>: forget the town's
            // "Receive now" record (all, or one distribution) once, when the town is first seen.
            static bool sForgot = false;
            if (const char* e = std::getenv("CF_DISTRIB_FORGET"); e && *e && !sForgot) {
                sForgot = true;
                forgetReceivedNow(std::strcmp(e, "1") ? e : "");
            }
#endif
        }
    }
    static const auto start = std::chrono::steady_clock::now();
    if (sTestNow >= 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= sTestNowAt &&
        nwc24::status().taskRegistered && sTownSeen) {
        std::lock_guard lk(sM);
        recomp::log("[distrib] CF_DISTRIB_NOW: pressing Receive now for %s", kDefs[sTestNow].key);
        receiveNow(sTestNow);
        sTestNow = -1;
        if (!sTestNowList.empty()) {
            sTestNow = sTestNowList[0].first, sTestNowAt = sTestNowList[0].second;
            sTestNowList.erase(sTestNowList.begin());
        }
    }
    // Live delivery: the game's own WiiConnect24 job (the one it starts when a town is loaded,
    // fn_801783B4 -> fn_8017B6F4) reads the stored package; Pete brings the letter.
    // A job we started is polled to its end with the game's own poll (fn_80178498: -1 while it
    // runs or waits for a retry, then its result), as the game polls the jobs it starts: the poll
    // (fn_8017803C) frees the job thread's stack once it has ended, and until then the game's
    // job starter (fn_80177F44) refuses every new job. Unpolled, the save's WiiConnect24 step
    // (fn_80178410, the HUD save / save and quit) could never start, so "Saving..." stayed up for
    // good (found 2026-11-04 with Nintendo events on, branch save-crash).
    static bool sJobPending = false;
    if (sJobPending) {
        const s32 r = s32(guestCall(c, VA(0x80178498)));
        if (r == -1) return;
        sJobPending = false;
        recomp::log("[distrib] the game's WiiConnect24 check ended (result %d)", r);
    }
    const u32 save = rd32(VA(0x8074E960));  // the town's save data (null before a town is loaded)
    const bool town = save >= 0x80000000u && save < 0x94000000u;
    static u32 sFrame = 0;
    if (++sFrame % 600 == 0 && std::getenv("CF_NWC24_LOG"))
        recomp::log("[distrib] frame %u: save %08X, town WiiConnect24 %d, task %d; game's check: state %d error %d/%d; "
                    "Nintendo letter slot %08X", sFrame, save, town ? rd8(save + 0x735E4u) : -1, nwc24::status().taskRegistered,
                    s32(rd32(VA(0x8074EBCC))), s32(rd32(VA(0x8074EBCC) + 4)), s32(rd32(VA(0x8074EBCC) + 8)), town ? rd32(save + 0xFC980u) : 0);
    if (const char* d = std::getenv("CF_DISTRIB_SAVE_DUMP"); d && town && sFrame == u32(std::atoi(d))) {
        std::error_code ec;
        fs::create_directories(folder(), ec);
        std::ofstream(folder() / "save_dump.bin", std::ios::binary).write(reinterpret_cast<const char*>(hostp(save)), 0x100000);
        recomp::log("[distrib] save data dumped (1 MiB at %08X)", save);
    }
    // A town is in play once the game has run its WiiConnect24 job for it (player select registers
    // the download tasks; the second one, prc_list.bin, is there for every town).
    const auto st = nwc24::status();
    if (!town || !st.anyTask) return;
    if (!rd8(save + 0x735E4u)) {
        // The town's own WiiConnect24 setting is off (a town started while WiiConnect24 was off):
        // turned on with the game's own setter (fn_80177DE0, what Rover's question and the attic
        // phone call; it sets the flag only while the game's WiiConnect24 check says "available").
        static bool sTried = false;
        if (port::settings().events.townWc24 && !sTried && rd32(VA(0x8074EBCC)) == 1) {
            sTried = true;
            const u32 r = guestCall(c, VA(0x80177DE0));
            recomp::log("[distrib] turned the town's WiiConnect24 setting on (fn_80177DE0 -> %u, flag %u)", r,
                        rd8(save + 0x735E4u));
        }
        return;
    }
    if (!port::settings().events.liveDelivery) return;
    // The game's job registers the distribution task (when a town is loaded with the setting on);
    // later it reads each package KD stored.
    static bool sRegistered = false;
    bool want;
    {
        std::lock_guard lk(sM);
        want = sState.syncWanted || (!sRegistered && !st.taskRegistered);
    }
    if (!want) return;
    if (guestCall(c, VA(0x801783B4))) {
        std::lock_guard lk(sM);
        sRegistered = true;
        sState.syncWanted = false;
        sJobPending = true;
        recomp::log("[distrib] started the game's WiiConnect24 check (live delivery)");
    }
}

std::string describeSchedule(long long secs2000, int n) {
    std::lock_guard lk(sM);
    std::ostringstream o;
    long long day = consoleDay(secs2000);
    for (int i = 0; i < n; i++) {
        const Slot s = loopSlot(day);
        const Date dt = civil(s.startDay);
        o << dt.y << "-" << dt.m << "-" << dt.d << " " << (s.dist >= 0 ? kDefs[s.dist].key : "-") << " (cycle " << s.cycle << ")\n";
        day = slotStart(s.index + 1);
    }
    return o.str();
}

namespace {
constexpr const char* kOnceNote =
    "\"Receive now\" gives each distribution once per town (recorded with your town's id and name\n"
    "from the save, so a copied data folder keeps it and another town has its own). The delivery\n"
    "schedule still brings every distribution again in its turn.";
}  // namespace

bool drawMenuPage() {
    bool dirty = false;
    auto& es = port::settings().events;
    ImGui::TextWrapped("Letters from Nintendo with the items and designs that were distributed over WiiConnect24 "
                       "from December 2008 to December 2012. They come the way they did on a Wii: the game "
                       "downloads them with WiiConnect24 and Pete brings the letter with the item to your house.");
    ImGui::TextDisabled("Distribution archive courtesy of WiiLink");
    ImGui::Spacing();
    static const char* kModes[] = {"Off", "In turn (every distribution, repeating)", "Original dates",
                                   "Only when I choose"};
    int m = es.mode;
    if (ImGui::Combo("Delivery", &m, kModes, 4)) {
        es.mode = m;
        dirty = true;
        if (m) fetchArchive();
        nwc24::requestDownload();
    }
    ImGui::SetItemTooltip(
        "Off (default): nothing is downloaded and WiiConnect24 stays off, as before.\n"
        "In turn: every distribution of your region comes once per cycle, evenly spaced by the game's\n"
        "clock (seasonal ones in their month), and the cycle repeats.\n"
        "Original dates: each one on the dates it was sent, by the game's clock (clock offset included).\n"
        "Only when I choose: just the ones you pick with \"Receive now\" below (each once per town).\n"
        "While on, WiiConnect24 is reported on to the game and the archive is downloaded from WiiLink's\n"
        "repository on GitHub (once; cached in the data folder). Turning it on or off applies at the next start.");
    if (es.mode == int(Mode::Loop)) {
        static const char* kPeriods[] = {"Every week", "Every 2 weeks", "Every month"};
        if (ImGui::Combo("How often", &es.loopPeriod, kPeriods, 3)) dirty = true;
    }
    if (es.mode == int(Mode::Original)) {
        dirty |= recomp::menu::Checkbox("Every year", &es.originalYearly);
        ImGui::SetItemTooltip("On: each distribution comes every year on its original month and day.\n"
                              "Off: only in its original year (2008-2012; set the clock offset to go back).");
    }
    if (es.mode) {
        dirty |= recomp::menu::Checkbox("Include other regions' distributions", &es.otherRegions);
        ImGui::SetItemTooltip("Also the ones sent only in other regions, when the item has a name in your game's language.\n"
                              "Their letters are the English ones from the archive.");
        dirty |= recomp::menu::Checkbox("Turn WiiConnect24 on for my town", &es.townWc24);
        ImGui::SetItemTooltip("A town started while WiiConnect24 was off has its own WiiConnect24 setting off, and the game\n"
                              "then ignores Nintendo's letters. On: the port switches it on with the game's own setting\n"
                              "(what Rover's question does), kept when you save.");
        dirty |= recomp::menu::Checkbox("Deliver while playing", &es.liveDelivery);
        ImGui::SetItemTooltip("Runs the game's own WiiConnect24 check (the one it makes when a town is loaded) as soon as\n"
                              "a letter is downloaded, so the game keeps it at once. Pete hands it over when you next\n"
                              "load your town (save first). Off: the game picks it up at that load itself.");
    }
    std::lock_guard lk(sM);
    loadState();
    if (es.mode) {
        const auto st = nwc24::status();
        ImGui::Text("Archive: %s", sFetchStatus.c_str());
        ImGui::Text("WiiConnect24: %s%s%s", st.taskRegistered ? "task registered" : "the game has not registered its task yet",
                    st.keyInstalled ? ", key installed" : "", st.vffPresent ? ", download box ready" : "");
        if (!st.lastResult.empty()) ImGui::TextWrapped("Last check: %s", st.lastResult.c_str());
        const long long day = consoleDay(consoleNow());
        if (es.mode == int(Mode::Loop)) {
            const Slot s = loopSlot(day), n = loopSlot(slotStart(s.index + 1));
            if (n.dist >= 0) {
                const Date d = civil(n.startDay);
                ImGui::Text("Now: %s. Next: %s on %04d-%02d-%02d (game clock).", s.dist >= 0 ? kDefs[s.dist].name : "-",
                            kDefs[n.dist].name, d.y, d.m, d.d);
            }
        }
        if (sState.pending >= 0) ImGui::Text("Waiting for WiiConnect24: %s", kDefs[sState.pending].name);
    }
    ImGui::Separator();
    if (es.mode) {
        const auto n = sState.names.find(sState.town);
        if (sTownSeen && n != sState.names.end())
            ImGui::TextDisabled("Town: %s. \"Receive now\" works once per distribution for each town.", n->second.c_str());
        else
            ImGui::TextDisabled("\"Receive now\" works once per distribution for each town (load your town first).");
        ImGui::SetItemTooltip("%s", kOnceNote);
#ifdef CF_DEV_TOOLS
        // Developer builds only (cfrecomp): never in the shipped CityFolkRecomp.
        ImGui::SameLine();
        if (recomp::menu::SmallButton("Forget received##distrib_dev")) forgetReceivedNow("");
        ImGui::SetItemTooltip("Developer build only: forgets this town's \"Receive now\" record, so every\n"
                              "distribution can be received again (testing). The schedule's record is kept.");
#endif
    }
    const TownState& t = sState.towns[sState.town];
    if (ImGui::BeginTable("##distribs", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH,
                          ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 16))) {
        ImGui::TableSetupColumn("Distribution");
        ImGui::TableSetupColumn("Original dates");
        ImGui::TableSetupColumn("State");
        ImGui::TableSetupColumn("##now", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (int i = 0; i < kDefCount; i++) {
            const Def& d = kDefs[i];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (const long long at = receivedNowAt(d.key)) {
                ImGui::TextDisabled("%s", d.name);
                ImGui::SetItemTooltip("%s%s\nReceived on %s (game clock) through \"Receive now\".", typeName(d.type),
                                      d.design && d.item ? " + design" : "", dateText(at).c_str());
            } else {
                ImGui::Text("%s", d.name);
                ImGui::SetItemTooltip("%s%s", typeName(d.type), d.design && d.item ? " + design" : "");
            }
            ImGui::TableNextColumn();
            if (d.unreleased) ImGui::TextDisabled("never distributed");
            else {
                std::string txt;
                for (const auto& w : windows(d)) {
                    char b[48];
                    static const char* kReg[] = {"NA", "EU", "JP", "KR"};
                    const int ri = w.region == 'E' ? 0 : w.region == 'P' ? 1 : w.region == 'J' ? 2 : 3;
                    std::snprintf(b, sizeof(b), "%s%s %04d-%02d-%02d", txt.empty() ? "" : ", ", kReg[ri], w.y0, w.m0, w.d0);
                    txt += b;
                }
                ImGui::TextWrapped("%s", txt.c_str());
            }
            ImGui::TableNextColumn();
            const bool region = distributedIn(d, gameRegion());
            if (!inArchive(d)) ImGui::TextDisabled("not in archive");
            else if (t.now.count(d.key)) ImGui::TextDisabled("received (Receive now)");
            else if (t.received.count(d.key)) ImGui::Text("received");
            else if (!archiveReady(d)) ImGui::TextDisabled(es.mode ? "downloading" : "available");
            else ImGui::Text("%s", region || d.unreleased ? "available" : "available (other region)");
            ImGui::TableNextColumn();
            if (inArchive(d)) {
                const long long nowAt = receivedNowAt(d.key);
                const bool onItsWay = sState.pending == i || sNowWaiting == i;
                ImGui::BeginDisabled(!es.mode || !archiveReady(d) || !sTownSeen || nowAt || onItsWay);
                if (recomp::menu::SmallButton("Receive now")) receiveNow(i);
                ImGui::EndDisabled();
                if (nowAt)
                    ImGui::SetItemTooltip("Received on %s (game clock).\n%s", dateText(nowAt).c_str(), kOnceNote);
                else if (onItsWay)
                    ImGui::SetItemTooltip("On its way: the game keeps the letter at its next WiiConnect24 check,\n"
                                          "Pete brings it when your town is next loaded.\n%s", kOnceNote);
                else if (es.mode && !sTownSeen)
                    ImGui::SetItemTooltip("Load your town first: \"Receive now\" is once per town.");
                else
                    ImGui::SetItemTooltip("Sends this distribution now. %s", kOnceNote);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Your own original WiiConnect24 files (.enc) can go into %s", port::displayPath(folder()).c_str());
    ImGui::SetItemTooltip("Files the Wii downloaded from Nintendo (rvforestdl_us.enc and the like). They are given to the\n"
                          "game as they are: KD checks Nintendo's signature with the game's key, so only genuine files work.");
    for (const auto& l : localFiles()) {
        const std::string p = l.path.string();
        ImGui::PushID(p.c_str());
        const long long at = receivedNowAt(encKey(l.data));
        ImGui::BeginDisabled(!es.mode || !sTownSeen || at || sState.pendingLocal == p);
        if (recomp::menu::SmallButton("Receive now")) {
            sState.pendingLocal = p;
            nwc24::requestDownload();
        }
        ImGui::EndDisabled();
        if (at) ImGui::SetItemTooltip("Received on %s (game clock).\n%s", dateText(at).c_str(), kOnceNote);
        else if (es.mode && !sTownSeen) ImGui::SetItemTooltip("Load your town first: \"Receive now\" is once per town.");
        ImGui::SameLine();
        auto res = sLocalResult.find(p);
        ImGui::Text("%s%s%s", port::displayPath(l.path.filename()).c_str(), res != sLocalResult.end() ? ": " : "",
                    res != sLocalResult.end() ? res->second.c_str() : "");
        ImGui::PopID();
    }
    return dirty;
}

bool selfTest(std::string* failure) {
    auto fail = [&](const std::string& w) {
        if (failure) *failure = w;
        return false;
    };
    for (int i = 0; i < kDefCount; i++) {
        const Def& d = kDefs[i];
        if (d.item && !archiveEntry(itemPath(d))) return fail(std::string("item not in archive list: ") + d.key);
        if (d.design && !archiveEntry(designPath(d))) return fail(std::string("design not in archive list: ") + d.key);
        if (!d.unreleased && windows(d).empty()) return fail(std::string("no dates: ") + d.key);
    }
    for (long long z : {daysFromCivil(2000, 1, 1), daysFromCivil(2012, 2, 29), daysFromCivil(2038, 12, 31)}) {
        const Date dt = civil(z);
        if (daysFromCivil(dt.y, dt.m, dt.d) != z) return fail("calendar");
    }
    auto pkg = u8Archive({{"a.bin", {1, 2, 3}}, {"b.bin", std::vector<u8>(40, 7)}});
    if (pkg.size() % 32 || pkg[0] != 0x55) return fail("u8");
    auto bmg = bmgLetter("Dear \n,", "Hi", "From me", "Nintendo", 64);
    if (bmg.size() % 32 || std::memcmp(bmg.data(), "MESGbmg1", 8)) return fail("bmg");
    return true;
}

}  // namespace recomp::distrib
