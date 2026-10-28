# Online play

City Folk's Nintendo Wi-Fi Connection features (town visits, chat, Wii Speak) and WiiConnect24
(letters) restored **between players of this port**: no Nintendo, Wiimmfi or WiiLink servers, no
console identities, no server of ours. User decision (2026-10-27): online does not need to work
with real consoles or revival services. Branch online-1.

Confidence tags: **[disc]** read from our discs / disassembly; **[run]** seen in a test run;
**[web]** from service pages and wikis (untrusted, unverified).

Contents: 1 what the game has; 2 the game's online flow (disassembly); 3 why DWC-level HLE;
4 DWC on rooms (online_dwc.cpp); 5 rooms and transport (online_net.cpp); 6 security;
7 reachability; 8 F1 > Online; 9 test aids and the two-instance test; 10 friends and mail
(design); 11 what is left; 12 decisions for the user.

## 1. What the game has

Nintendo WFC (ended May 20, 2014 [web]): **town visits** of up to 4 players (owner + 3), friends
only (each side registers the other's 12-digit friend code); text chat with the on-screen or a
USB keyboard; **Wii Speak** voice chat (the first Wii Speak game); everyone hears a speaker even
without their own Wii Speak (manual p. 22). WiiConnect24 (ended June 2013 [web]): letters to
friends' towns, Wii Message Board messages, Nintendo item distributions.

On the disc [disc]: RVL DWC (`<< RVL_SDK - DWC release build: Sep 17 2008 >>`) with GameSpy
(GP presence/buddies, QR2, server browser, NAT negotiation, GT2 transport), DWC_VCT 2.0.2 (voice
chat), PMIC (Wii Speak), KBD, SSL, NHTTP, NWC24. GameSpy game name **`acrossingwii`**, secret key
`Z7Fm9K`, product id 0x2CC0 (all three passed to DWC_InitFriendsMatch), match key
`str_key='rvl_forest'`. Hosts `naswii.nintendowifi.net` (NAS login/profanity), `gpcm/gpsp.gs.`,
`%s.available/master/ms%d/natneg1-3.gs.nintendowifi.net`, `gamestats(2).gs.` (all unused now:
section 3). WiiConnect24: `/shared2/wc24/*`, `nwcs.wapp.wii.com/ruu/rvforestdl_us.enc` (item
distribution), `wus.wapp.wii.com/notify|inquiry`.

## 2. The game's online flow [disc]

Read from the disassembly (scratch tools: callers of every DWC/GameSpy function, string
references, call graphs); names in `config/RUUE01/names_manual.txt` (51 DWC functions from
their `!!X() was called!!` traces and their use), applied to all four versions by
`tools/discovery/apply_manual.py` (all matched by hash, LoginAsync by order). No game code
outside DWC calls GameSpy, SO sockets, SSL or NHTTP directly; RELs never call DWC.

Layers (USA addresses):

| Layer | Where | What |
|---|---|---|
| Copper (gatekeeper REL `d_a_npc_sp_gatekeeper_aNP`) and the friend roster menus (`d_menu_friend*NP`) | RELs | call the online system: `fn_800DCBC8` connect, `fn_800DCC54` open the gate (host), `fn_800DCCEC(i)` visit friend *i*, `fn_800DCE44` disconnect; roster: `fn_800E5A04` = DWC_CreateFriendKeyToken (register a code), `fn_800E5B24` = DWC_CheckFriendKey |
| Online system (`r13-0x3188`, 0x3868 bytes) | `fn_800E4A08` setup (user data = save+4, friend list = save+0x44, 32 friends, 4 players), `fn_800E4ECC` start | copies the save's DWC user data (0x40) and friend list (32 x 12) to working copies, starts the network thread; request queue +0x3678 (1 anybody, 2 host, 3 join friend, 4 cancel, 5 close all, 6 reset); per-friend mirror +0x1C40 (0xD0 each); state +0x385C (1 idle, 3 connecting, 5 hosting, 6 visiting) |
| Network thread (`fn_800E7C20`, object `r13-0x3174` = 0x805B67E0) | `fn_800E7xxx`-`fn_800E91D4` | the only DWC caller (below) |
| DWC | 0x80319698-0x8033F46C | friends match, matching, transport, account |
| GameSpy / SO / NHTTP / SSL | 0x8033F46C-0x8036xxxx, 0x8043xxxx | under DWC only |
| VCT (voice) | 0x80365xxx-0x8036Dxxx, game side `fn_8017xxxx` | sends over DWC_SendUnreliable, receives through the game's DWC receive callback (`fn_800E9164` hands it to VCT first) |

Network thread, in order:
1. `NWC24TrySuspendScheduler` (retried while busy) - WiiConnect24 paused while online.
2. allocate (heap 0x6000), `SOInit` (alloc/free callbacks) and `SOStartup` (errors -> WFC error
   via `fn_80412BFC`, an NCD/IOS error code).
3. `DWC_InitFriendsMatch(NULL, userdata, 0x2CC0, "acrossingwii", "Z7Fm9K", 0x4000, 0x4000,
   friendList, 32)`; `DWC_SetSendSplitMax(1329)`.
4. `DWC_LoginAsync(name, NULL, fn_800E8E1C, 0)`; the callback sets `DWC_SetConnectionClosedCallback`,
   `DWC_SetUserSendCallback`, `DWC_SetUserRecvCallback`, `DWC_SetUserPingCallback`,
   `DWC_SetTransportTimeout(180000)`, `DWC_SetBuddyFriendCallback`, `DWC_SetOwnStatusData`
   (0x66 bytes of game status), `DWC_ClearDirtyFlag(userdata)` (the new profile id goes to the save),
   then `DWC_UpdateServersAsync` (friend status callback -> the game's roster; delete callback).
5. Every frame (`fn_800E8264`): request dispatch, queued reliable sends
   (`DWC_IsSendableReliable` / `DWC_SendReliable` per AID, queue of 0x24-byte items),
   `DWC_ProcessFriendsMatch`, VCT, friend/roster upkeep (`DWC_CanChangeFriendList`,
   `DWC_DeleteBuddyFriendData`), `DWC_GetLastErrorEx` -> game error states, `DWC_ClearError`;
   every 180 frames a ping per AID (`DWC_Ping`, 40 missed -> `DWC_CloseConnectionHard`); client
   matching timeout 180 s (`DWC_GetLastMatchingType() == 3` -> `DWC_CancelMatch`).
6. Host (request 2): `DWC_SetupGameServer(topology 2, 4, matched fn_800E8F30, 0, newClient,
   0, attempt fn_800E8EF0, NULL, 0)`. The attempt callback asks the game (`fn_800DDF78`: not while
   busy).
7. Visit (request 3, friend *i*): `DWC_GetFriendStatusDataSC(friend[i], &max, &num, NULL,
   &size)` must return status 6 (server) with num < max (else errors 15/16), then
   `DWC_ConnectToGameServerAsync(2, i, ...)`.
8. Matched callback: AID bitmap rebuilt from `DWC_GetMyAID` / `DWC_IsValidAID`, receive buffers
   per AID (`DWC_SetRecvBuffer`), the game's join callback; closed callback: bit cleared, game
   callback, and with fewer than 2 hosts left either back to waiting (server) or
   `DWC_CloseAllConnectionsHard`.
9. `DWC_RequestSuspendMatchAsync` while the game must not take visitors; `DWC_CancelMatch`
   closes the gate; `DWC_ShutdownFriendsMatch`, `SOCleanup`, `SOFinish`,
   `NWC24ResumeScheduler`, `DWC_Shutdown` on the way out.
10. `DWC_ConnectToAnybodyAsync(2, 4, "str_key='rvl_forest'", ...)` after
    `DWC_AddMatchKeyString("str_key", "rvl_forest")` is linked (request 1) but no game path was
    found that issues it (visits are friends only).

Account functions (pure computations on the 0x40-byte DWCUserData and 12-byte DWCFriendData)
run as game code: friend data word 0 bits 11-12 type (1 login id, 2 friend key, 3 GS profile id),
bit 13 buddy; word 1 the profile id (types 2 and 3), word 2 the friend-key checksum (type 2).
Friend key = checksum << 32 | profile id, checksum = MD5(profile id, game code)[0] >> 1 (so
codes are region-specific: RUUE / RUUP / RUUJ / RUUK). User data: +0x10 authentic login id
(type 1 = logged in once), +0x1C GS profile id, +0x20 dirty flag, +0x24 game code, +0x3C CRC.

## 3. Why DWC-level HLE

The game's own visiting, gate, roster, chat and VCT code calls only the public DWC API; nothing
outside DWC touches the network. Replacing that API (about 50 functions) means:
- the game's flow and dialogue stay exactly as on the Wii (Copper, friend roster, errors);
- GameSpy, NAS, SSL, NAT negotiation and console identities never need emulating, nor do SO
  sockets (SOInit/SOStartup/SOCleanup/SOFinish and the WC24 scheduler pause only answer "ok"
  while in a room);
- the account functions keep computing real friend codes from our profile ids.
The IOS-device approach of the earlier research (host sockets under SO, rewrite hostnames to a
revival service) is not needed any more. Not covered by DWC HLE: Wii Speak (PMIC/USB device),
the USB keyboard, WiiConnect24 letters (NWC24): section 11.

## 4. DWC on rooms (src/recomp/online_dwc.cpp)

- **Login** succeeds only in a room (else error 20100, "join or host a room in F1 > Online").
  The profile id is this install's (online.cpp: HMAC of a random 32-byte identity in
  `<data>/online/identity.bin`), written into the game's user data as the authentic login id
  (+0x10 type 1, +0x1C profile id, dirty flag): the game saves it and shows its own friend code,
  which F1 shows too (`DWC_CreateFriendKey`).
- **Friends**: a friend-list entry is a buddy when a room member has that profile id and has
  ours in its own list (both registered each other in the game's roster, as WFC required).
  `DWC_UpdateServersAsync` then turns friend-code entries into buddy profile entries (as DWC did
  after GP confirmed the buddy) and reports statuses; status changes reach the game's friend
  status callback. Status 1 online, 5 visiting, 6 hosting (with max/num players) and the game's
  0x66 bytes of status data (`DWC_SetOwnStatusData`) go to every room member (STATUS message
  with our friend-list profile ids, resent when the list changes).
- **Matching**: the gate's host is the DWC server (AID 0); a visitor's JOIN request is checked
  (server role, not suspended, room left, friends both ways, the game's attempt callback), gets
  the next AID and the node list; other nodes get the new list. Matched / new-client / closed
  callbacks as DWC calls them; a departing server ends the match for everyone; losing the room
  while online is network error 80430 for the game (its own "connection lost" path).
- **Transport**: `DWC_SendReliable` / `DWC_SendUnreliable` by AID over the room (reliable in order,
  up to 64 KB per message here; the game sends at most its split size), receive callback with
  the per-AID receive buffer, send callback on the next process, `DWC_Ping` round trip.
- Matches are an overlay on the room: a room may hold four players in two towns' worth of
  sessions only one at a time per player (DWC allows one match per console).

## 5. Rooms and transport (src/recomp/online_net.cpp)

Serverless (user requirement): **the hosting player's game is the room**. Members connect
straight to the host over UDP; the host relays between members (star), enforces sender ids
and forwards only within the room.
- **Room code**: Crockford base32 of [version, 80-bit random room secret, up to 4 endpoints
  (IPv4/IPv6 + port: router-mapped public address, STUN public address, LAN address, global
  IPv6), checksum byte], in groups of five, e.g. `240G4-0R40M-30E20-9182C-P03H0-XTDD2-G` (one
  IPv4 endpoint). A new code every time a room is hosted; the joiner tries every endpoint, the
  first answer wins. The password is never in the code.
- **LAN rooms**: the host broadcasts a beacon every 2 s on UDP 29911 (name, host, game id,
  version, players, lock, port, room secret); F1 lists rooms heard in the last 6 s.
- **Direct connect**: IP:port (port forwarding, Tailscale, ZeroTier, Hamachi). Allowed unless
  the host turns it off; then only the code works.
- **Handshake**: HELLO (game id, port version, name, nonce, room id from the secret, profile id)
  -> CHALLENGE (host nonce, password flag, PBKDF2 salt + iterations) -> AUTH (proof) -> WELCOME
  (sealed) or WAIT (host asked) or REJECT (reason text).
- **Sealed packets**: ChaCha20 + HMAC-SHA256/128 (encrypt-then-MAC) under a per-join session key,
  one key per direction, 64-packet replay window. Reliable messages: sequence numbers,
  selective acks, retransmission (RTT-based, backoff), window 64, in-order delivery,
  fragmentation (1100-byte fragments, 256 KB per message); unreliable messages up to 1000 bytes;
  pings every second, 15 s timeout, BYE on leave.
- **Members**: up to 4 (City Folk's limit), ids 0 (host) to 3; member list with names, profile ids,
  friend codes and pings, sent by the host on every change.
- **Version check**: same online protocol, same port version, same game id. Towns visit only
  within one region (USA with USA, Europe with Europe, Japan with Japan, Korea with Korea): DWC's
  friend codes embed the game code, the save formats and text encodings differ, and Nintendo WFC
  kept the regions apart too. All European languages share RUUP01.
- Tested by `cf_online_test` (tools/tests/online_net_test.cpp, ctest `online_net`): codes
  (round trip, lower case, typo detection), no / wrong / right password, direct connect, other
  region refused, 200 KB reliable message relayed member to member, host broadcast, unreliable,
  room full, kick, leave, room closed, ask before join (accept / decline), old code, LAN list.

## 6. Security and privacy

- No accounts. A random per-install identity; profile ids are derived from it (31 bits).
- The room secret (in the code) and the password are both needed to derive the session key:
  master = HMAC(secret, "cf-room" | PBKDF2-HMAC-SHA256(password, salt, 100000)), proof and
  session key = HMAC(master, label | nonces). The password is never sent or saved; a wrong one
  is refused ("Wrong password or room code"); 5 failures from one address within a minute block
  it for a minute. No forward secrecy: anyone with the code (and password) who recorded the
  traffic could decrypt it. LAN beacons carry the room secret (anyone on the LAN can join a room
  without a password, by design).
- Kick: the member is dropped and refused for the rest of the room (by profile id and address).
  "Ask me before someone joins": the host gets a notice with Let in / Decline.
- Limits: datagrams over 1400 bytes dropped, 16 handshakes in flight, 4 members, 256 KB per
  message, 4096 queued messages, every length checked before use, text from the network
  stripped of control characters and cut to 32 characters.
- What others see: your player name, profile id / friend code, your IP address (the host sees
  every member's; members see only the host's), and what the game itself sends (your town while
  visited, chat). Nothing is stored by anyone but the game's own save. No server.
- STUN asks a public STUN server (Google, Cloudflare) for the host's public address when port
  mapping is on; nothing else leaves the room.
- Crypto written from the specifications (FIPS 180-4, RFC 2104, 8018, 8439) with known-answer
  self tests; the OS random generator (BCryptGenRandom / getrandom / arc4random).

## 7. Reachability

Hosting, with "Map the port on my router" (default on): UPnP-IGD (SSDP search, the router's
WANIPConnection/WANPPPConnection AddPortMapping UDP, 4 h lease, removed on Close room) and, if
that fails, NAT-PMP (gateway UDP 5351); STUN from the room's own socket for the public address.
F1 shows the result: "Reachable from the internet at a.b.c.d:29910 (checked through the router)"
when a test packet sent to the public address comes back (router hairpin), "Port mapped ... could
not be checked from here" when the router does not loop packets back, "Not reachable from the
internet: <why>" otherwise (also double NAT / carrier-grade NAT: the router reports a private
address). Fallbacks, documented in F1: friends on the same network (LAN list), a manual port
forward (UDP 29910) + Direct connect, or a VPN (Tailscale, ZeroTier, Hamachi) + Direct connect
with the VPN address. Global IPv6 addresses go into the code too (no NAT; the host's firewall
must allow the port: Windows asks the first time). Not done: NAT hole punching without a server
(both players exchanging codes; section 11).

## 8. F1 > Online

Status (room, game: logged in / hosting / visiting, your friend code), player name, then:
- **Host a room**: room name, password (optional), UDP port, ask before join, map the port on
  the router, show on the local network, allow direct connections, Host. While hosting: the
  code with Copy, LAN / internet addresses, reachability, member list with ping and Kick, join
  requests (Let in / Decline), Close room.
- **Join with a code** (+ password), **Rooms on this network** (locked rooms marked; other
  regions greyed out), **Direct connect** (IP:port + password, with the port-forward / VPN
  help).
- While in a room: the member list (friend codes, so players can register each other in the
  game's roster) and Leave.
- Room notices over the game (joined / left / refused, join requests with buttons).
Settings `[online]` in settings.ini (player_name, room_name, port, ask_before_join, allow_direct,
lan_visible, port_mapping, last_direct, notices); passwords are never saved.

## 9. Test aids and the two-instance test

- `CF_ONLINE_LOG=1`: every DWC call and room event. `CF_ONLINE_NAME=<name>`.
- `CF_ONLINE_HOST=<port>` hosts at start (no port mapping unless `CF_ONLINE_UPNP=1`;
  `CF_ONLINE_PASSWORD`), `CF_ONLINE_CODE_FILE=<file>` gets the code; `CF_ONLINE_JOIN=<code>|
  <ip:port>|file` joins (file = wait for the code file). `CF_ONLINE_PID_FILE=<file>` writes this
  install's profile id.
- `CF_ONLINE_TEST=<frame>:<action>;...` (USA only, game thread): `connect`, `host` (open the
  gate), `visit:<friend index>`, `disconnect` (the online-system calls Copper makes), and
  `friend:<slot>:<pid>|@<file>` (writes a profile-id entry into the save's friend list, as
  registering a friend code would).
- Transport alone: `build/windows-clang/cf_online_test.exe`.

Two-instance run (2026-10-27, this PC, two scratch APPDATA/NAND/cache folders, both through
`tools/routes/town.txt` into a new town): see section 11 for the result.

## 10. Friends and mail (design; not implemented yet)

User request: a friend system in F1 and letters between friends at any time, replacing
WiiConnect24 letters.
- **Identity**: each install gets an Ed25519 key pair (from the existing identity seed; a small
  public-domain implementation such as TweetNaCl's or our own from RFC 8032, plus X25519 for
  encryption). The F1 friend code is a short base32 of the public key's hash (with the player
  and town name for display). The DWC profile id becomes a hash of the public key, so the
  game's own 12-digit friend code identifies the same key, and a peer proves it in the room
  handshake by signing the nonces (collisions in 31 bits are then detected, not trusted).
- **Adding friends**: in a room's member list, "Add friend" sends a friend request over the room
  (the key comes with it: nobody copies codes); the other player gets an accept/decline notice
  (also listed under F1 > Online > Friends as pending); accepting makes both friends. Requests
  rate-limited (e.g. 3 per minute per member), ignorable and blockable. Member list shows who is
  a friend already. Codes can still be typed for friends met elsewhere.
- **The game's roster**: F1 friends are written into the save's DWC friend list as buddy profile
  entries **through the game's own registration path** (the call the roster menu makes after a
  code is typed: `d_menu_friend03NP`/`friend08NP` -> `fn_800E5A04` DWC_CreateFriendKeyToken and
  the name fields of the roster entry, to be traced), never by editing other save fields; friends
  removed in F1 are removed in the game with `DWC_DeleteBuddyFriendData`. Then both can visit each
  other directly when online (presence below), and rooms stay for quick sessions.
- **Presence without a room**: each friend publishes its current endpoint (signed, encrypted to
  each friend) where friends look: the BitTorrent mainline DHT (BEP 44 mutable item keyed by a
  per-friendship value) or a direct UDP ping to the last known endpoint. When both are online the
  game connects directly (the same transport, a two-member room).
- **Mail**: the game's letters to Wii friends go through NWC24 (`NWC24CommitMsg`, message box
  files under `/shared2/wc24/mbox`, MIME with the letter as an attachment; the game's letter
  format and attached item to be traced in the NWC24 calls of the letter scene). HLE the send:
  the committed message goes into our outbox (encrypted to the recipient's X25519 key, signed);
  delivery: directly when both are online; otherwise store-and-forward in the DHT (BEP 44:
  1000-byte values, signed by the sender's key, keyed by recipient + sequence; the sender
  re-publishes every ~30 minutes until the recipient acknowledges). Incoming letters are written
  where the game's WiiConnect24 receive path reads them (`wc24recv.mbx` + control file), so Pete
  delivers them as he did WiiConnect24 letters. Only friends can mail you; per-friend rate limits;
  letters never in plain text on the wire.
- **Reliability, honestly**: direct delivery is reliable whenever both play at the same time.
  DHT items live about 2 hours unless re-published; a sender's game re-publishes only while it
  runs, so a letter between two players who are never online at the same time arrives when the
  recipient starts the game within ~2 hours of the sender's last session, else later when they
  overlap. Mainline DHT nodes may drop or not store items (it works in practice for BEP 44 users,
  but nothing guarantees it). A self-hostable mailbox server (any friend group can run one, the
  F1 setting names it) would make offline delivery dependable; optional, later.

## 11. Status and what is left

Done (branch online-1): the DWC-level HLE (51 named functions), the room transport (tests pass),
port mapping + STUN + reachability, F1 > Online, test aids. See CLAUDE.md status for the
two-instance result.

Left:
- **Wii Speak voice chat** with the PC microphone: PMIC / the USB device under it
  (`/dev/usb/oh0`) are absent, so VCT never records; the transport already carries VCT packets
  (unreliable, the game hands them to VCT). Needs a PMIC-level HLE (SDL3 audio capture, opt-in,
  push-to-talk) and testing the VCT streaming between two instances.
- **USB keyboard** for chat (KBD library on `/dev/usb/kbd`): host keyboard events.
- **WiiConnect24 letters** between towns: section 10.
- **Friends system and presence**: section 10.
- **NAT hole punching** without a server: both players exchange codes (each code carries the
  STUN endpoint), then both send to each other at once; works for most home routers
  (endpoint-independent mapping), not for symmetric NAT. Not done; port mapping, LAN and VPN
  cover the rest.
- **Optional lobby/relay server** (self-hostable, lowest priority): room codes without
  reachable hosts, relaying for symmetric NAT. Not written.
- Region: PAL/Japan/Korea use the same HLE by name (tables carried); the CF_ONLINE_TEST actions
  are USA-only.

## 12. Decisions for the user

- Default UDP port 29910 (and 29911 for LAN beacons): fine, or another?
- STUN servers (Google, Cloudflare) are contacted when hosting with port mapping on: acceptable,
  or should that be its own checkbox?
- Friend codes: keep the game's own registration (both type each other's code in the game's
  roster, as on the Wii; works now), or also write F1 friends into the roster automatically
  (section 10)?
- Mail without a common online time: DHT store-and-forward (best effort) or a hosted mailbox
  server (dependable, someone runs it)?
- Who hosts an optional public relay/lobby, if ever (none needed for rooms).
