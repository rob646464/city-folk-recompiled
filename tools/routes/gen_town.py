# Builds tools/routes/town.txt: the scripted route from boot to the town (USA, new game), for
#   cfrecomp --input @tools/routes/town.txt   with CF_INPUT_CLOCK=frames
# (docs/WII.md "Scripted route to the town"). One item per press; frame numbers are guest frames.
import os

items = []


def press(frame, tokens="A", hold=6):
    items.append(f"{frame}:{tokens}*{hold}")


def point(frame, x, y):
    items.append(f"{frame}:@{x}/{y}*1")


# Wrist strap, title ("Press A"), Rover's first lines.
for f in range(1300, 4601, 80):
    press(f)
# "Start fresh!" / "That's right!" choices.
point(4620, 0.41, 0.42)
for f in range(4660, 7561, 60):
    press(f)

KEYS = {"r": (-0.339, 0.017), "o": (0.364, 0.017), "b": (0.052, 0.292)}
CONFIRM = (0.656, 0.778)


def keyboard(start, word):
    f = start
    for ch in word:
        point(f, *KEYS[ch])
        press(f + 10)
        f += 40
    point(f, *CONFIRM)
    press(f + 10)
    return f + 40


f = keyboard(7600, "rob")
# Dialogue after the name, then the town-name keyboard; A on "That's it!" style choices.
cycle = [(0.62, 0.5), (0.41, 0.42), CONFIRM]
end_dialogue = f + 1500
i = 0
while f < end_dialogue:
    point(f, *cycle[i % 3])
    press(f + 10)
    f += 60
    i += 1
f = keyboard(f, "rob")
while f < 16500:
    point(f, *cycle[i % 3])
    press(f + 10)
    f += 60
    i += 1

here = os.path.dirname(os.path.abspath(__file__))


def write(name):
    out = os.path.join(here, name)
    with open(out, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(",\n".join(items) + "\n")
    print(out, len(items), "items")


write("town.txt")
base = list(items)
# town_walk.txt: the same, then walking in the town from frame 16700 (pointer away so the
# HUD is not clicked): 90-frame legs up / right / down / left with a pause, repeated to ~21000.
items.append("16690:@off*1")
f = 16700
legs = ["STICKUP", "STICKRIGHT", "STICKDOWN", "STICKLEFT"]
while f < 21000:
    for leg in legs:
        items.append(f"{f}:{leg}*90")
        f += 110
write("town_walk.txt")
# grass_walk.txt (docs/FEATURES.md "Grass wear"): town.txt, A presses for the map tutorial that
# follows the arrival, then the same square (left, down, right, up from the bus stop) run with B
# held (60-frame legs) and walked (80-frame legs, about the same distance), alternating, to ~21000.
items[:] = base
items.append("16690:@off*1")
for f in range(16700, 17101, 60):
    press(f)
f = 17200
while f < 21000:
    for b, n in (("+B", 60), ("", 80)):
        for leg in ("STICKLEFT", "STICKDOWN", "STICKRIGHT", "STICKUP"):
            items.append(f"{f}:{leg}{b}*{n}")
            f += n + 10
write("grass_walk.txt")
