"""Gecko code for Dolphin reference runs: the port's warps (docs/WARPS.md) inside Dolphin.

    py tools/dolphin/make_warps.py            -> prints the "$Places warps" code (RUUE01.ini)

One C0 (execute) block, PowerPC encoded here (no assembler needed). Every frame the code handler
runs it: once the player stands at the house door (actor pointer 0x805D26E4, position 1200, 84,
640: the continue's arrival in the clean fixture) a frame counter starts; at each entry's count the
code calls the game's own stage change fn_80163064(stage manager 0x805FB218, stage, &pos, type,
angle, fadeOut 2, fadeIn 2), the call the port's warps make (arguments from a port run with
CF_WARP_LOG=1 and tools/routes/dolphin_places_warp.txt, same order and spacing). USA rev 1 only.
The counter, the state and the table live inside the code block (Dolphin writes the code list
once), reached with the bl / mflr trick.
"""
import struct

# (frames after the arrival, stage, type, angle, pos) = the port's CF_WARP_LOG "go to stage" lines
WARPS = [
    (600, 0, 0x0B, 0x0000, (1880.0, 28.0, 2000.0)),   # river below the waterfall (FIELD)
    (1500, 30, 0x4A, 0x8000, (256.0, 0.0, 464.0)),    # RM_MM_ENT museum lobby
    (2400, 35, 0x4A, 0x4000, (48.0, 0.0, 416.0)),     # RM_MM_PIC0 paintings
    (3300, 31, 0x4A, 0x8000, (512.0, 0.0, 464.0)),    # RM_MM_FISH aquarium
    (4200, 37, 0x4A, 0xC000, (432.0, 0.0, 384.0)),    # RM_MM_CAFE The Roost
    (5100, 38, 0x81, 0x8000, (256.0, 0.0, 464.0)),    # RM_MM_ASTRO observatory
    (6000, 27, 0x4A, 0x8000, (256.0, 0.0, 464.0)),    # RM_OFFICE town hall
    (6900, 39, 0x4A, 0x8000, (256.0, 0.0, 464.0)),    # RM_SHOP0 Nook's Cranny
    (7800, 44, 0x4A, 0x8000, (256.0, 0.0, 464.0)),    # RM_TAILOR Able Sisters
    (8700, 49, 0x4A, 0x8000, (256.0, 0.0, 464.0)),    # RM_GRACE GracieGrace
    (9600, 47, 0x4A, 0x8000, (256.0, 0.0, 464.0)),    # RM_BARBER Shampoodle
    (10500, 48, 0x4A, 0x8000, (256.0, 0.0, 464.0)),   # RM_FORTUNE Katrina
    (11400, 46, 0x4A, 0x8000, (256.0, 0.0, 464.0)),   # RM_BROKER Crazy Redd's
    (12300, 50, 0x4A, 0x8000, (256.0, 0.0, 464.0)),   # RM_HAPPY_ENT HRA office
    (13200, 54, 0x4A, 0x8000, (256.0, 0.0, 464.0)),   # RM_AUCTION Lyle's auction house
    (14100, 52, 0x4A, 0x8000, (256.0, 0.0, 464.0)),   # RM_THEATER_ENT Club LOL
]


def D(op, d, a, imm): return (op << 26) | (d << 21) | (a << 16) | (imm & 0xFFFF)
def addi(d, a, i): return D(14, d, a, i)
def li(d, i): return addi(d, 0, i)
def lis(d, i): return D(15, d, 0, i)
def ori(a, s, i): return D(24, s, a, i)
def lwz(d, off, a): return D(32, d, a, off)
def stw(s, off, a): return D(36, s, a, off)
def stwu(s, off, a): return D(37, s, a, off)
def mulli(d, a, i): return D(7, d, a, i)
def cmpwi(a, i): return D(11, 0, a, i)
def cmpw(a, b): return (31 << 26) | (a << 16) | (b << 11)
def cmplw(a, b): return (31 << 26) | (a << 16) | (b << 11) | (32 << 1)
def add(d, a, b): return (31 << 26) | (d << 21) | (a << 16) | (b << 11) | (266 << 1)
MFLR0, MTLR0, MTCTR12, BCTRL, BLR = 0x7C0802A6, 0x7C0803A6, 0x7D8903A6, 0x4E800421, 0x4E800020
def mflr(d): return (31 << 26) | (d << 21) | (8 << 16) | (339 << 1)

# branches resolved after layout: ("bc", BO, BI, label) / ("bl", label)
BNE, BEQ, BLT, BGE = (4, 2), (12, 2), (12, 0), (4, 0)

data = [0, 0, 0, 0]  # counter, started, next index, pad
for count, stage, typ, angle, (x, y, z) in WARPS:
    data += [count, stage, typ, angle] + [struct.unpack(">I", struct.pack(">f", v))[0] for v in (x, y, z)] + [0]
data += [0] * 8  # end marker (count 0)

# Own 32-byte frame; LR and r31 saved inside it (16, 20): the code handler keeps state at its own
# stack words, so nothing is written above our frame (its +4 is for the game function we call).
def stmw(s, off, a): return D(47, s, a, off)
def lmw(d, off, a): return D(46, d, a, off)
def mfcr(d): return (31 << 26) | (d << 21) | (19 << 1)
def mtcrf(s): return (31 << 26) | (s << 21) | (0xFF << 12) | (144 << 1)
# Every register the code or the game function touches is saved in its own 160-byte frame (r3-r31
# with stmw, LR, CR): the code handler keeps its state in registers and its stack.
prog = [MFLR0, stwu(1, -160, 1), stw(0, 8, 1), mfcr(0), stw(0, 12, 1), stmw(3, 16, 1), ("bl", "after")]
data_at = len(prog)
prog += data
labels = {"after": len(prog)}
prog += [mflr(31),
         lwz(3, 4, 31), cmpwi(3, 0), ("bc",) + BNE + ("counting",),
         lis(4, 0x805D), lwz(4, 0x26E4, 4),
         lis(5, 0x8000), cmplw(4, 5), ("bc",) + BLT + ("done",),
         lis(5, 0x8180), cmplw(4, 5), ("bc",) + BGE + ("done",),
         lwz(5, 0x64, 4), lis(6, 0x4496), cmpw(5, 6), ("bc",) + BNE + ("done",),
         lwz(5, 0x6C, 4), lis(6, 0x4420), cmpw(5, 6), ("bc",) + BNE + ("done",),
         li(3, 1), stw(3, 4, 31)]
labels["counting"] = len(prog)
prog += [lwz(3, 0, 31), addi(3, 3, 1), stw(3, 0, 31),
         lwz(5, 8, 31), mulli(6, 5, 32), add(10, 31, 6), addi(10, 10, 16),
         lwz(7, 0, 10), cmpwi(7, 0), ("bc",) + BEQ + ("done",),
         cmpw(3, 7), ("bc",) + BLT + ("done",),
         addi(5, 5, 1), stw(5, 8, 31),
         lis(3, 0x805F), ori(3, 3, 0xB218),
         lwz(4, 4, 10), lwz(6, 8, 10), lwz(7, 12, 10), addi(5, 10, 16), li(8, 2), li(9, 2),
         lis(12, 0x8016), ori(12, 12, 0x3064), MTCTR12, BCTRL]
labels["done"] = len(prog)
prog += [lwz(0, 12, 1), mtcrf(0), lmw(3, 16, 1), lwz(0, 8, 1), MTLR0, addi(1, 1, 160), BLR]
assert data_at == 7

words = []
for i, w in enumerate(prog):
    if isinstance(w, tuple):
        if w[0] == "bl":
            words.append((18 << 26) | (((labels[w[1]] - i) * 4) & 0x3FFFFFC) | 1)
        else:
            _, bo, bi, lab = w
            words.append((16 << 26) | (bo << 21) | (bi << 16) | (((labels[lab] - i) * 4) & 0xFFFC))
    else:
        words.append(w & 0xFFFFFFFF)
if len(words) % 2:
    words.append(0x60000000)  # nop: the block is whole 8-byte lines, ending in blr + nop
import sys
if "--nocall" in sys.argv:
    words = [0x60000000 if w == BCTRL else w for w in words]
out = ["$Places warps", f"C0000000 {len(words) // 2:08X}"]
out += [f"{words[i]:08X} {words[i + 1]:08X}" for i in range(0, len(words), 2)]
print("\n".join(out))
