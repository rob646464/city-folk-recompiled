# Scripted DSU (cemuhook) server: one DS4-like pad in slot 0, for driving Dolphin's emulated
# Wii Remote from a timed script (reference captures). Protocol written from the cemuhook
# protocol description (as our src/recomp/dsu.cpp client).
#   dsu_server.py <port> <script> <seconds> <log>
# Script lines: <t0 s> <dur s> <tokens...>  tokens: A B 1 2 PLUS MINUS HOME UP DOWN LEFT RIGHT C Z
#   ptr=x,y (KPAD-like -1..1, y down; held until the next ptr=) stick=x,y (-1..1, y up)
import socket, struct, sys, time, zlib

port, script_path, total, log_path = int(sys.argv[1]), sys.argv[2], float(sys.argv[3]), sys.argv[4]
items = []
for line in open(script_path, encoding="utf-8"):
    p = line.split()
    if not p or p[0].startswith("#"):
        continue
    items.append((float(p[0]), float(p[1]), p[2:]))
log = open(log_path, "w", encoding="utf-8")

SERVER_ID = 0x5C17F00D
BTN1 = {"MINUS": 0x01, "PLUS": 0x08, "UP": 0x10, "RIGHT": 0x20, "DOWN": 0x40, "LEFT": 0x80}
BTN2 = {"C": 0x04, "Z": 0x08, "2": 0x10, "B": 0x20, "A": 0x40, "1": 0x80}


def packet(msg_type, body):
    data = bytearray(b"DSUS" + struct.pack("<HHII", 1001, len(body) + 4, 0, SERVER_ID) + struct.pack("<I", msg_type) + body)
    struct.pack_into("<I", data, 8, zlib.crc32(data) & 0xFFFFFFFF)
    return bytes(data)


SLOT_INFO = bytes([0, 2, 2, 1]) + bytes([0x00, 0x11, 0x22, 0x33, 0x44, 0x55]) + bytes([5])


def state_at(t):
    b1 = b2 = home = 0
    ptr = (0.0, 0.0)
    stick = (0.0, 0.0)
    for t0, dur, toks in items:
        for tok in toks:
            if tok.startswith("ptr=") and t >= t0:
                x, y = tok[4:].split(","); ptr = (float(x), float(y))
        if t0 <= t < t0 + dur:
            for tok in toks:
                if tok in BTN1: b1 |= BTN1[tok]
                elif tok in BTN2: b2 |= BTN2[tok]
                elif tok == "HOME": home = 1
                elif tok.startswith("stick="):
                    x, y = tok[6:].split(","); stick = (float(x), float(y))
    return b1, b2, home, ptr, stick


def axis(v):
    return max(0, min(255, int(round(128 + v * 127))))


sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.1", port))
sock.setblocking(False)
client = None
t_start = None
pkt = 0
last = None
end = time.time() + total + 600  # waits up to 10 min for the client
while True:
    now = time.time()
    if now > end or (t_start is not None and now - t_start > total):
        break
    try:
        while True:
            data, addr = sock.recvfrom(2048)
            if len(data) < 20 or data[:4] != b"DSUC":
                continue
            mt = struct.unpack_from("<I", data, 16)[0]
            if mt == 0x100000:
                sock.sendto(packet(0x100000, struct.pack("<H", 1001)), addr)
            elif mt == 0x100001:
                n = struct.unpack_from("<i", data, 20)[0]
                for i in range(max(0, min(n, 4))):
                    slot = data[24 + i]
                    info = bytes([slot, 2 if slot == 0 else 0, 2, 1]) + SLOT_INFO[4:] + b"\0"
                    sock.sendto(packet(0x100001, info), addr)
            elif mt == 0x100002:
                if client is None:
                    log.write(f"client {addr} at {now:.3f}\n"); log.flush()
                client = addr
                if t_start is None:
                    t_start = now
    except BlockingIOError:
        pass
    except ConnectionResetError:
        pass
    if client is not None:
        t = now - t_start
        b1, b2, home, ptr, stick = state_at(t)
        st = (b1, b2, home, ptr, stick)
        if st != last:
            log.write(f"{t:8.3f} b1={b1:02X} b2={b2:02X} home={home} ptr={ptr} stick={stick}\n"); log.flush()
            last = st
        body = bytearray(SLOT_INFO + b"\x01" + struct.pack("<I", pkt))
        body += bytes([b1, b2, home, 0, axis(stick[0]), axis(stick[1]), axis(ptr[0]), axis(-ptr[1])])
        body += bytes([255 if b1 & 0x80 else 0, 255 if b1 & 0x40 else 0, 255 if b1 & 0x20 else 0, 255 if b1 & 0x10 else 0])
        # analog face buttons in protocol order: Square (Y), Cross (B), Circle (A), Triangle (X)
        body += bytes([255 if b2 & 0x80 else 0, 255 if b2 & 0x40 else 0, 255 if b2 & 0x20 else 0, 255 if b2 & 0x10 else 0])
        body += bytes([255 if b2 & 0x08 else 0, 255 if b2 & 0x04 else 0, 0, 0])
        body += bytes(12)  # touches
        body += struct.pack("<Q", int(t * 1e6))
        body += struct.pack("<fff", 0.0, -1.0, 0.0) + struct.pack("<fff", 0.0, 0.0, 0.0)
        try:
            sock.sendto(packet(0x100002, bytes(body)), client)
        except OSError:
            pass
        pkt += 1
    time.sleep(0.004)
log.write("done\n")
