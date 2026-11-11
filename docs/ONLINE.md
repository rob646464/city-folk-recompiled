# Online play

City Folk's Nintendo Wi-Fi Connection features (town visits, chat, Wii Speak) and WiiConnect24
(letters) restored **between players of this port**: no Nintendo, Wiimmfi or WiiLink servers, no
console identities. User decision (2026-10-27): online does not need to work with real consoles or
revival services. Branch online-1. Since branch online-lobby (2026-11-10, section 13) rooms are found
in a **room list** served by a small free lobby (a Cloudflare Worker, or self-hosted), which also helps
two routers connect and hands out a relay as the last resort; rooms themselves stay peer to peer, and
the room / reply codes of sections 5-10 are the "Advanced" fallback.

Confidence tags: **[disc]** read from our discs / disassembly; **[run]** seen in a test run;
**[web]** from service pages and wikis (untrusted, unverified).

Contents: 1 what the game has; 2 the game's online flow (disassembly); 3 why DWC-level HLE;
4 DWC on rooms (online_dwc.cpp); 5 rooms and transport (online_net.cpp); 6 security;
7 reachability; 8 F1 > Online; 9 test aids and the two-instance test; 10 friends and mail
(design); 11 what is left; 12 decisions for the user; 13 the lobby (room list, connecting two
routers, the relay, deploy guide, privacy, tests).

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
  while in a room; outside one SOStartup fails at once with SO_ETIMEDOUT and NHTTPStartup always
  fails: the save's WiiConnect24 step uses them, docs/NAND.md "The save's network step");
- the account functions keep computing real friend codes from our profile ids.
The IOS-device approach of the earlier research (host sockets under SO, rewrite hostnames to a
revival service) is not needed any more. Not covered by DWC HLE: Wii Speak (PMIC HLE, section
10e), the USB keyboard (section 10b), WiiConnect24 letters (NWC24): section 11.

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

(As built on branch online-1. The page is now built around the room list: section 13.5; what is
described here lives under "Advanced".)

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
- `CF_ONLINE_TEST=<frame>:<action>;...` (game thread). USA only: `userdata` (creates the save's
  DWC user data the way the game does when it writes the save, if missing: a never-saved new town
  has none and the online system refuses to start), `authid` (the save's user data gets the
  authentic login id a first WFC connection gives, so Copper skips issuing a friend code),
  `setup` (`fn_800DCBC4`, Copper calls it first),
  `connect`, `host` (open the gate), `visit:<friend index>` (polled every frame until it returns 1,
  as Copper does), `disconnect`, `diag` (the start conditions), `start`, and
  `friend:<slot>:<pid>|@<file>` (a profile-id entry in the save's friend list at save+0x44, as
  registering a friend code would). Any version: `addfriend:<member id>`, `acceptfriends`,
  `friends`. With `CF_ONLINE_LOG` the online system's / network thread's / session states are
  logged on change, and the first reliable sends/receives.
- Transport and crypto alone: `build/windows-clang/cf_online_test.exe` (ctest `online_net`).

Two-instance runs (2026-10-27/28, this PC, scratch APPDATA/NAND/cache per instance):
- New towns from `tools/routes/town.txt` (never saved): the online system refused to start until
  the `userdata` step (the save's DWC user data is created with the first save).
- `CF_FIXTURE=user-town` + `tools/routes/continue.txt` on both (the same town twice):
  room joined (LAN address), both games log in (`[dwc] logged in`, profile ids from the keys,
  the game's friend codes), `UpdateServers` turns the profile-id entries into buddies, the host
  opens the gate (`SetupGameServer`, network thread state 9), the visitor sees status 6 (1/4),
  `ConnectToGameServer` -> both matched (thread state 10), the visitor's `fn_800DCCEC` returns 1 on
  the 4th poll and both online systems reach session state 3; the host's game then sends a
  2-byte message every frame and the visitor receives them (3100+ in a minute). The visitor's
  HUD disappears (trip prepared) but no trip starts and no town data moves: the rest of the visit
  is driven by Copper's conversation (REL `d_a_npc_sp_gatekeeper_aNP`), which the test actions
  skip. Next step: a route that walks both players to the gate and talks to Copper ("Host" /
  "Visit"), and a second, different town (the same town on both sides may be refused by the game).
- F1 friends between two instances at the title: keys proved both ways, `addfriend:0` ->
  "Bob sent you a friend request" on the host -> accepted -> "Alice accepted your friend request",
  both `friends.txt` list each other as friends.
- Roster entries (2026-11-02, `user-town` host + `user-town2` visitor, the user's two towns): after
  the F1 request/accept each game gets the other in its own roster ("friend Bob (profile id ...)
  -> game roster slot 0: registered"), and a forced save afterwards completes (result word 1).
- The real gate flow (2026-11-02): the town gate is at the same place in both towns; walking into
  its entrance (`CF_TELEPORT=<f>:1790,84,664`, then the stick up, with running) loads the
  gatehouse (Copper = module 66 actor 0x75, Booker = 67 / 0x76, around (320,304) / (192,304)).
  Talking to Copper (pointer off: `@off`, A) gives his menu "I wanna go out! / Invite guests! /
  Friend code! / Where am I? / Oh, nothing." Through our DWC HLE: "I wanna go out!" -> "Nintendo
  WFC." -> (no friend code yet) "would you like to be issued a friend code?" -> Yes -> saving ->
  "Now connecting to Nintendo WFC" -> the game logs in (our profile id becomes its friend code,
  e.g. 3141-4534-8795), updates its friend statuses and shuts DWC down again, exactly as with
  WFC; with the code already in the save (test step `authid`) both instances get to "Go to which
  town?" (empty while nobody hosts). The Remote D-pad and the stick do not move a menu
  highlight: the pointer has to hover the line (and stay there through the A press).
  Route timing: loading a save takes a CPU-load-dependent number of frames, so the scripts must
  be anchored to module links (`after link d_a_npc_sp_gatekeeper_aNP`, the town list
  `after link d_menu_friend03NP`), not absolute frames.
- **The visit works end to end (2026-11-03)**: host `user-town` ("Frunch") picks "Invite guests!"
  -> "Nintendo WFC." (SetupGameServer, the gate opens, online state 5); the visitor `user-town2`
  picks "I wanna go out!" -> "Nintendo WFC.", Copper lists "Rob64 Frunch" with 1/4 players,
  pointer + A on the row, Confirm -> "Submitting an inquiry to rob64..." -> the protocol below
  runs -> saving -> the visitor walks out of Frunch's gate, both games show both players.
  The game's own visit protocol over DWC reliable data (2-byte header: 0x80 | bits 5-6 | 0x10 =
  control | type, then a per-connection sequence byte; fn_800D4260 receive, fn_800D6070 send,
  fn_800D3654 sequence check): hello (type 0) / reply (type 1) both ways, a few control messages,
  the visitor's player data (type 5, 34542 bytes), the host's town data (type 7, 5 x 92162 +
  17455 bytes), then the in-play messages (types 0xD..0xF, 12-180 bytes). What it took:
  (1) friends' roster entries with names (INFO, section 10) - an entry without names is listed
  blank and cannot be picked; (2) the joining client's matched callback a few frames after its
  AID is set (the game copies DWC_GetMyAID into +0x366E once per frame and its connect handler
  fn_800D43D8 skips the AID equal to that copy; same-frame = still 0 = the server, so the visitor
  never greeted the host); (3) no new-client callback on the joining client; (4) one reliable
  send per connection until its send callback, as in DWC; (5) reliable messages up to the
  transport's 256 KB. Logs: `[dwc] <wall s> SendReliable / receive` with the first bytes.
  Scripts: scratch gate_host.txt / gate_vis2.txt (continue.txt, teleport to the gate, walk in,
  Copper, pointer hovers); the host's A presses must stop once the gate is open (more A cancels
  hosting).

## 10. Friends and mail (friends done; roster, presence and mail are design)

User request: a friend system in F1 and letters between friends at any time, replacing
WiiConnect24 letters.
- **Identity (done)**: each install's Ed25519 key (seed = HMAC of the identity file; our own
  SHA-512/Ed25519/X25519, src/recomp/online_ed25519.cpp, RFC test vectors pass). F1 friend code =
  base32 of SHA-256(public key)[0..10], `XXXX-XXXX-XXXX-XXXX`. The DWC profile id is
  HMAC("cf-dwc-profile-id", public key) (31 bits), so the game's own 12-digit friend code belongs to
  the key; a member whose room profile id does not match its key is not trusted.
- **Adding friends (done, src/recomp/online_friends.cpp)**: members send IDENT (key, nonce, name)
  and PROVE (signature over the peer's nonce) to each other on joining; the member list shows
  "checking key..." / **Add friend** / Friend / Request sent / Accept request / Blocked. A request
  is a notice with Accept / Decline / Block (also in F1 > Online > Friends); accepting (or both
  asking) makes both friends. Requests only from proved keys, at most one per 20 s and 5 per room
  session per member, none from blocked keys. Friends in `<data>/online/friends.txt` (state, key,
  name). Codes can be shown/copied; typing a code for a friend met elsewhere needs presence
  (below).
- **The game's roster (done)**: F1 friends go into the game's roster through its own
  registration call `cf_RosterRegisterOffline(slot, friendData, info)` (fn_8017C294; only while
  the game is offline, online system state <= 1), friend data = GS profile id entry. The friend
  info (0x78 bytes, save+0x1C8 per slot) is the one the friend's own game sends (room message
  INFO 0x45), in the layout the host's game itself writes for a visitor during a visit (read
  from a save after a real visit, 2026-11-03): +0x00 the Wii number letters go to, +0x08 a copy of
  the town name struct save+0x7EC2 (u16 id, 9 UTF-16 units, byte +0x14 = 0x01), +0x1E a copy of
  the player name struct save+0x7ED8 (byte +0x14 = 0x00), +0x36 0x42 bytes = save+0x10C8. The
  letter writer's "Dear <name>," / "To <name>" reads +0x1E. Builds before 2026-11-03 had the two
  structs the other way round (letters said "Dear Frunch,"): such entries from friends.txt are
  swapped on receipt. Registration waits for
  the INFO while the friend is in the room; an entry registered without it is updated later.
  Kept in friends.txt (`info <key> <hex>`). To do: removal from the game when removed in F1
  (`DWC_DeleteBuddyFriendData`). Then both can visit each
  other directly when online (presence below), and rooms stay for quick sessions.
- **Offline letters (user decision, 2026-11-03)**: no DHT, and the relay stores no letters (it
  stays an optional route for rooms only). Two paths:
  - **A. "Always online mail"** (F1 > Online, off by default): each player enters only their own
    email account; letters go as encrypted, signed attachments by SMTP and are fetched by IMAP
    (WiiConnect24's own mail model). Friends' addresses travel with the friend data (encrypted, in
    the room). Credentials only in the OS credential store; TLS with OS certificate checks;
    presets for the common providers (app passwords with 2-step verification; Microsoft personal
    accounts not supported, OAuth only). Fixed subject tag, a short human-readable body, the letter
    as attachment; imported only when signed by a known friend's key; poll at start-up and every
    few minutes on a background thread; "Send test letter to myself".
    Implementation (src/recomp/online_email.cpp, online_tls.cpp): subject "[City Folk letter] from
    <player>", body "This is a letter from <player>, sent by Animal Crossing: City Folk
    Recompiled", attachment letter.cfl = "CFEM" 1 | from key | to key | the letter sealed for the
    pair and signed. In: UID SEARCH on that subject, BODY.PEEK[] (stays unread), only an attachment
    addressed to us from a friend's key that opens is taken; then UID MOVE to "City Folk" (created)
    when the server has MOVE, else the UID is remembered locally; any other mail is never touched.
    TLS: SChannel on Windows (system chain + host name checks), implicit TLS on 993/465, STARTTLS on
    143/587 (refused if not offered). Password: Windows Credential Manager ("CityFolkRecompiled/
    mail/<address>"); on other systems this build stores nothing and says so (Keychain /
    libsecret and the TLS there: to do). Friends' addresses: room message EMAIL 0x49 (friends only),
    kept in friends.txt. F1 > Online "Letters while your friend is away".
    Tested 2026-11-03 against tools/tests/cf_mail_test_server (in-memory SMTP+IMAP, SChannel TLS
    with a self-signed CN=localhost certificate; test-only trust override CF_MAIL_TEST_CERT_SHA256,
    environment only): STARTTLS and implicit TLS (CF_MAIL_TEST_IMPLICIT) both send, fetch, import
    and move the mail; without the override the self-signed certificate is refused ("not
    trusted"), with a wrong pin too. Two instances: addresses exchanged in a room, Alice's letter
    sent while Bob was not running, Bob's game took it in at its next start. Not testable here: a
    host-name mismatch on a trusted certificate (would need our certificate in the Windows root
    store) and real providers (the user will try one). Test aids CF_MAIL_PASSWORD, CF_MAIL_ON,
    CF_MAIL_POLL_S, CF_ONLINE_TEST=<f>:mailself / mailtest (a 64-byte test letter, not a game one).
  - **B. "Deliver whenever both games are running"** (default without email): friends' games
    connect in the background whenever both are open and swap outbox letters. Limits: both games
    must run at the same time, and it needs automatic port opening (UPnP / NAT-PMP) or an earlier
    direct connection; otherwise letters wait until the next visit.
    Implementation (online_mail.cpp): UDP letters link on port 29913 (the install keeps the port it
    used last, link_port.txt), UPnP / NAT-PMP mapped when "Map the port" is on; its addresses (LAN
    + router public) go to friends in rooms (ENDPOINTS 0x48). An outbox letter for a friend not in
    a room with us is sealed for the pair and sent in 1000-byte chunks every 20 s until the
    friend's game acknowledges it; only proved friends' letters that open are taken. Tested
    2026-11-03 with two instances on one PC: delivered and acknowledged.
  With email on, both run; a letter id is taken in once.
- **Pelly's "Other town." (found 2026-11-03)**: Pelly's destination list is built by
  d_a_npc_sp_perikoNP fn_34_3794: "<town>." (message 0x18), "Other town." (0x19) only when town
  flag 0x1F is set (fn_801165F4(town data, 0x1F); town flags at town data +0x735C3, town data =
  fn_8010DD60 / r13-0x2FE0), "Future self." (0x1A), "Never mind." (0x1B). Choosing it then needs:
  1. The console's WiiConnect24 on (NWC24 check fn_80404BBC: SCGetWCFlags bit 0, else NWC24 error
     -39 and the game's message "The WiiConnect24 setting is not turned on" <109139>) and a tested
     network configuration (NCD, else "An Internet connection cannot be established" <109133>).
     The port now always reports both (SYSCONF NET.WCFG = 1, KD and NCD devices; nwc24_dl.cpp
     consoleWc24()); KD downloads still run only with Nintendo events on, and nothing goes on the
     network otherwise. A/B: CF_WC24_CONSOLE=0 (the old behaviour: on only with Nintendo events).
  2. The town's own WiiConnect24 setting (save +0x735E4, the attic telephone's option; else Pelly:
     "You can't send letters to people from out of town because your WiiConnect24 settings are
     turned off"). User decision 2026-11-03: the port turns it on once per town, the first time it
     sees the town with WiiConnect24 available, with the game's own setter fn_80177DE0, and records
     the town in `<data>/town_wc24.txt`, so turning it off on the attic telephone stays off.
     A/B: CF_TOWN_WC24_DEFAULT=0. Checked with a town without friends and Nintendo events off
     (user-town fixture): no message, saves complete, no KD download, no network use.
     **Town key (fixed 2026-11-04, branch distrib-once)**: the town is recorded by its own
     identity from the save, `<town id>-<town name as UTF-16 hex> <readable name>` per line
     (`src/recomp/town_id.cpp`, the same key as the Nintendo events record, docs/DISTRIBUTIONS.md
     "Receive now once per town": town data `rd32(0x8074E960)`, resident slots at +0x20 + slot *
     0x86C0, town struct at +0x7EC2 of the first resident that exists). Before, the key was read
     at town data + 0x7EC2, which is empty (0x20 bytes before the first resident's town struct; the
     residents' data starts at +0x20; `town_wc24.txt` files of earlier runs hold only zeros), so every town got the same all-zero key: the first town seen was recorded, and every
     other town on that install was then left alone, so one town's choice (left on, or turned off
     on the attic telephone) counted for all. Old entries (all-zero, or anything not in the
     `<id>-<name>` form) are ignored, so each town gets its default once again and is then
     recorded under its own key; a town turned off on the attic telephone before this fix gets
     turned on once more (the old file cannot tell which town that was). Not done while in an
     online room (the town data may be another player's town during a visit). Verified
     (2026-11-04, scratch folders): a `town_wc24.txt` holding only the old all-zero line, Nintendo
     events off, `user-town2`: "setting of test (A056): turned on for this town (first start)",
     line `A056-0074006500730074 test (A056)` added; with `user-town-clean` and the same old file
     `F177-004600720075006E00630068 Frunch (F177)` added, then test's own line in the same file.
  3. Flag 0x1F. The game's only setter is in the player-creation scene module
     (d_a_npc_sp_player_createNP fn_25_BB4, scene 0x3D; a new game is scene 0x3C); no town we have
     has it and a visit does not set it (a visit sets flag 0x16 in the visitor's town). The port sets
     it once when an F1 friend is in the town's roster (rosterSync). Not for every town: the extra
     choice would show for players who cannot address a letter to anyone.
  With these, Pelly answers "You'd like to send mail to another town, correct?" and takes the
  letter ("OK, I'll be happy to take your outgoing mail."). Test aids: CF_TOWN_FLAG=<frame>:<hex>
  (now on the town data, not the save file object), CF_TOWN_WC24=<frame> (the setter at a frame),
  CF_MAIL_PROBE=1 (Pelly's / the WiiConnect24 checks' results, scene id and flag 0x1F changes).
- **When the letter leaves (2026-11-03)**: Pelly only keeps it (save letter slots). The game sends
  her outbox in the save flow's WiiConnect24 step (save menu fn_801BC9E4 -> fn_801031B8 reset,
  fn_801031F4 until done: fn_80103064 walks the outbox, fn_8017816C starts one send task per
  letter, fn_8017ADD0 -> ... -> NWC24CommitMsg, our HLE: sealed letter in the outbox). So a letter
  goes when the player saves (save icon, bed, save and quit). Test aid CF_WC24_SEND=<frame> runs
  that step. Then path B carries it; the receiver's game takes it in at its next start
  (NWC24 search/read HLE, the game's own start-up WiiConnect24 job) and Pete delivers it.
  Verified both ways with the real visit pair (tester -> rob64, rob64 -> tester): queued, sent,
  "delivered over the letters link", "received", next start "taken by the game", the mailbox's
  new-mail flag. Not yet seen on screen: the letter opened from the mailbox (route not written).
- **Dedupe across paths**: a letter has one id whatever path carries it (room, letters link,
  email). The receiver keeps the ids it took in `<data>/online/mail/seen.txt` (latest 4096), so a
  later copy by another path, or a resend after a lost acknowledgement, is dropped even after the
  game has taken the first out of the inbox; the letters link acknowledges duplicates too (the
  first ack may have been lost). The sender drops a letter from its outbox on the first ack (room
  or link) or once email has sent it, so the other paths stop.
- **"Could not access Wii system memory" after a visit**: seen once (2026-11-03, before the
  WiiConnect24 changes) when the visitor came home. Two complete visits since (user-town2 ->
  user-town and back, the game's return-home save; console WiiConnect24 on, and A/B with
  CF_WC24_CONSOLE=0) showed no message: the WiiConnect24 send task at the return
  (fn_8017C59C -> fn_80178110, entry fn_8017AB90) fails quietly with -39 when WiiConnect24 is off
  and succeeds when on, and the saves complete. Not reproduced; if a player sees it, CF_SAVE_LOG=1
  CF_NAND_TRACE=1 CF_MAIL_PROBE=1 show the failing step.
- **Test runs without Resetti**: start each run from a copy saved by the game (the visit's
  return-home save, or the fixtures), and let a receiving game stay at the title (the letters link
  runs from boot, no town needs to load), so its next start is clean.
- Routes: tools/routes/visit_host.txt + visit_guest.txt (the visit, env in their headers),
  letter_other_town_test.txt / letter_other_town_frunch.txt (write to the friend, Pelly, send).
- **Letters link HELLO**: datagram type 3 (sender key, time, its addresses, HMAC with the pair's
  letter key) to every friend's known addresses at start and every 10 minutes; the receiver keeps
  the source address first (a changed port or address is learned without a room) and answers once.
## 10a. Settings while visiting (src/recomp/online_sync.cpp)

User request: a visitor's game uses the HOST's settings for everything that shapes the shared town
or could make the two games disagree; at home its own settings are back exactly as they were.
- **Exchange**: the host sends a versioned ini blob (`cfsync 1`, then `[section]` / `key=value`
  lines as settings.ini has them) right before it accepts the visitor into the match (room
  message M_SETTINGS, before JOIN_OK, so it is in effect before the game loads the town). The
  visitor answers with its own `[skin_tone]` lines. Unknown keys of a newer format are ignored.
- **Override**: `port::beginSettingsOverride` applies the synced keys in memory; `saveSettings`
  keeps writing the player's own values for them (settings.ini never holds the host's, so a quit
  or crash mid-visit loses nothing). `endSettingsOverride` puts the own values back when the
  match ends for any reason: back home, the connection or the room dropped, DWC shut down or
  logged in again. Changes made during the visit to non-synced options stay.
- **F1**: while visiting, the synced options (Enhancements > Gameplay: grass wear, hemisphere,
  real-time weather, region content; Online > Nintendo events) are greyed out under "Using
  <host>'s settings while visiting"; the skin tone stays editable.
- **Skin tones**: each player's chosen tone is its own (never synced); the tones travel with the
  blobs and the other game shows them on that player (cf_skin remote tones, in memory only;
  the player's own choices win). The host gets every visitor's tone.

| Setting (ini section / key) | While visiting | Why |
|---|---|---|
| `[calendar] hemisphere` | host | seasons, events, fish/bugs of the town |
| `[region]` events, flags, autumn moon, parents' days, villager houses | host | what the town holds and celebrates |
| `[region] title_screen` | own | title logo only |
| `[weather]` real_time, place, lat/lon, country | host | the town's weather comes from the host's place (not the visitor's location) |
| `[gameplay] grass_wear` | host | the ground of the town |
| `[skin_tone]` | own (shown on the other game) | user rule |
| `[nintendo_events]` mode, loop period, yearly, other regions | host | event schedule of the town |
| `[nintendo_events]` live delivery, town WiiConnect24 | own | the player's own save and mailbox |
| `[system]` clock offset, language, sensor bar, nickname, ... | own | each console keeps its own clock, as on Wii; the game uses the host's town time for the visit |
| `[video]`, `[rendering]`, `[enhanced_lighting]`, `[enhanced_world]`, `[crt]`, `[texture_packs]` | own | presentation only |
| `[audio]`, `[controls]`, `[wiimote]`, `[wiimote_hid]`, `[dsu]`, `[interface]`, `[updates]`, `[online]` | own | local |
| Luigi's Mansion leftovers in the shared table (`[enhancements]`, `[debug]`, other `[gameplay]` keys, `[silly_*]`, `[unused2]`, `[ghost_lights]`, `[world_lights]`) | own | not used by City Folk |

Test (2026-11-03, verified): host settings.ini `grass_wear=1`, `hemisphere=south`, tone 3 for its
player (F177-8AA5); visitor `grass_wear=2`, `north`, tone 7 (A056-DB16). The visitor logs
`[sync] visiting Alice: using the host's world settings (1 tones)` with the host's lines
(CF_ONLINE_LOG=1); both games draw the visitor with palette 6 and the host with palette 2
(CF_SKIN_LOG=1: the visitor's player data is the copy at USA 0x805D2720, player number 1); the
visitor walks back into the gatehouse, talks to Copper, goes home: `[sync] visit over: own
settings back` with `grass_wear=2` / `north`, and its settings.ini never changed.

Saves: the visitor's game saves when it leaves for the trip ("Saving...", result word 1) and
again when it is home (result words 0 -> 1 after the return), with the DWC layer shut down.

Menu navigation in Copper's online screens (F1 > Controls "Snap pointer to menu items", Auto):
works with the D-pad: his choices are 'B_msg' panes, the town list (d_menu_friend03NP) is found
as drawn panes (B_msg_00 row, B_scrlBtn arrows, B_cmnBtnA_00 Quit / _02 Confirm). The first
press lands on the item nearest the pointer, the next ones move: DOWN (row), A (pick), DOWN,
DOWN (Quit), RIGHT (Confirm), A visits. The list slides in for a few frames after it opens.

Removing (or blocking) a friend in F1 queues its profile id (`removed <pid>` in friends.txt);
when a town save is loaded and the game is offline, rosterSync takes it out of the game's roster
with the roster menu's own delete (`cf_RosterDeleteOffline`, fn_8017C3C0: later entries move up).
Verified with `CF_ONLINE_TEST=24000:removefriends` after a visit: `profile id ... removed from
the game roster (slot 0): done`.

## 10d. Hole punching (no server of ours)

For a host that is not reachable (no UPnP / NAT-PMP, no port forward) and a joiner behind an
ordinary home router: the joiner asks a public STUN server for its own public address while it
connects; after 3 s without an answer F1 shows it as a **reply code** (the room code format with
one endpoint and no secret). The joiner sends that code to the host by chat; the host enters it
under "Let them in": for 30 s the host sends small T_PUNCH packets to that address while the
joiner keeps sending HELLO to the host's public address (from its STUN, in the room code). Both
routers then have outgoing traffic to the other side and let the answers in (endpoint-independent
mapping, the usual home case); the join then proceeds as usual. It does not get through
symmetric NATs (some mobile and corporate networks): there the relay (10c) or a VPN helps.
While a reply code is shown the joiner waits 90 s instead of 10 s. Test: cf_online_test checks
that the host's punch packets arrive at the address of a reply code. Untested between two real
separate networks (the user will try it with a friend).

## 10c. The optional self-hosted relay (tools/relay/cf_relay.cpp)

Nobody has to run a server; a friend group that wants one runs `cf_relay [--port 29912]
[--mail-dir <folder>]` on a machine everyone can reach (a VPS, a home server with the port
forwarded) and enters its address in F1 > Online "Relay (optional)" (`[online] relay`,
CF_ONLINE_RELAY for tests).
- **Rooms**: a host with a relay registers its room there under a random 8-byte room id every 10 s
  and adds a relay endpoint to its room code (family 0x54: relay IPv4 + port + room id), next to
  its direct endpoints; joiners try all routes at once. Through the relay the members have
  virtual addresses (100::/64 discard prefix, the relay's peer id in the last four bytes), so the
  room transport above is unchanged: its sealed packets are wrapped in `CFRL` TO_HOST / TO_PEER
  datagrams and the relay only forwards them between that room's host and its joiners (it cannot
  read them; it only knows room ids and addresses). Limits: 256 rooms, 16 joiners per room,
  600 datagrams/s per address, rooms expire 60 s after their host stops registering.
- **Letters**: none (user decision): the relay forwards room traffic only.
- Test (2026-11-03): `cf_online_test` joins a room through an in-process relay with a code that
  has only the relay route and sends 50 KB.

## 10b. USB keyboard chat (src/recomp/ios_devices.cpp)

City Folk takes chat text (and letters, names, ...) from a USB keyboard (its KBD library over IOS
`/dev/usb/kbd`). The option is F1 > Controls > Ports "Type with the PC keyboard (disables keyboard
game controls)" (`[keyboard] type_with_pc_keyboard`, off by default; docs/CONTROLS.md "Typing with
the PC keyboard"; until branch pc-keyboard it was F1 > Online "USB keyboard", `[online]
usb_keyboard`, read once for migration). The device is always there (the game's KBD library opens it
once at boot and never retries: 1.x's option, which made the open fail while off, did nothing when
turned on later, the user's "Don't think it's working"); while the option is on a keyboard is
attached. The module's async ioctl 0 gets 16-byte messages (public IOS interface): `{u32 type: 0
attached, 1 detached, 2 keys; u32 keyboard id; u8 modifiers (HID bits); u8 0; u8 keys[6] (HID
usages)}`, one per key press / release from the SDL key events (pc_keyboard.cpp ->
`ios::usbKeyboardReport`; SDL scancodes are HID usages). The KBD library's writes to the device
(LEDs) are answered -4 as before. Keyboard bindings are off while typing (mouse and controllers
work).

Fix (branch online-voice): the first key report crashed the game (`std::_Throw_Cpp_error` in
`UsbKeyboard::ioctl`): answering the pending request runs the KBD library's callback at once on
the same thread, and that callback queues the next request (ioctl -> the keyboard mutex, still
held). Requests are now answered after the mutex is released.

Verified 2026-11-03 in a real visit (host `Frunch-rob64`, visitor `test-tester`, copies of the
test_builds profiles): with the keyboard on, typing starts the game's chat input; Enter sends.
The visitor's "Hello rob" shows as a bubble over the visitor in its game and as "tester Hello
rob" in the host's chat strip (top right) while the host stood in the gatehouse. It also works offline (the bubble shows
in one's own town). Test aids `CF_KBD_TYPE="<guest frame>:<text>|..."` and the route token
`KEYS=<text>` (src/recomp/pc_keyboard.cpp; since branch pc-keyboard real SDL key events through
SDL's queue, so the bindings see them too; syntax in docs/CONTROLS.md "Typing with the PC
keyboard"), `CF_KBD_LOG=1`.

## 10e. Wii Speak voice chat (src/recomp/online_voice.cpp)

User request: voice chat from the PC microphone, played through the game's own path.

**How the game does it [disc]** (USA addresses): the Wii Speak is driven by Nintendo's PMIC
library (`<< RVL_SDK - PMIC release build: Sep 18 2008 >>`, USB 057E:0308 on `/dev/usb/oh0` via
the IUSB library). The network thread starts it (`fn_800E7E90` -> PMICInit `fn_80225CE4`) and
runs the game's Wii Speak state machine `fn_80172E20` every frame: PMICProbe `fn_802260CC`
(0 attached, -2 none) each frame, then Open `fn_802261A0` -> control requests `fn_80229B50`
(0), `fn_8022A15C` (3) -> Start `fn_80226358`; Stop `fn_8022646C` / Close `fn_802262C8` when it
closes. Async calls take `(cb, arg)`; the game sets a "waiting" word before each call and polls
it every frame, the callback stores the result. The device stays closed until a voice
conference exists: when a player is in the visit, the online system's request mask (+0x36DC,
set by `fn_800E5754` from the visit protocol) makes the network thread call `fn_80171B94(aid)`
-> VCT conference add `fn_8036A978`; VCT's event (7) -> VCT_StartStreaming `fn_80366244`. Then a
periodic alarm `fn_8017169C` every VCT frame (144 ms): VCT's mixed incoming voices
(`fn_80366B2C`) -> soft limiter `fn_80171554` -> an AX voice the game streams (`fn_801721E0`),
so **everyone hears the others with or without a Wii Speak**; PMICRead `fn_80228FD4` (1152
samples = 8 kHz) -> the game's echo suppressor `fn_80229D08` (mic frame vs the voices playing)
-> VCT_SendAudio `fn_803664E8` (only with a session in state 2) -> voice detection
`fn_8036CCF4` -> `_VCT` packets of 912 bytes over `DWC_SendUnreliable` (`fn_803675A4`), which
the DWC HLE carries over the room (sealed, section 6); received ones go to VCT through the
game's DWC receive callback (`fn_800E9164` -> `fn_8036D398`). With a Wii Speak the HUD shows a
grey microphone icon at the top right. An AI DMA callback (`fn_801723BC`) feeds the game's own
output back to PMIC as an echo reference.

**The port**: the PMIC API is answered by the host (hooks.txt "Wii Speak", USA names, `all`
versions; tables matched in PAL/JP/KR): Init/Quit 0; Probe 0 while F1 > Online "Microphone" is
on, else -2 (a Wii without a Wii Speak: the game still plays the others' voices); Open/Close/
Start/Stop and the control requests answer 0 with the callback called at once; Read gives 1152
samples from the microphone (SDL3 recording stream, mono s16 at 8 kHz, at most ~2 frames of
latency kept) after the input gain, or 0 when there are none / push-to-talk is up; the echo
reference resampler/writer return 0 / n (no speaker next to a PC microphone). The game's VCT,
VAD, packet format and playback are untouched.
- **Voice detection needs a noise floor.** VCT ignores frames with energy < 49 and starts sending
  only on a frame ~5x louder (rms) than the last four frames above that (`fn_8036CCF4`, threshold
  520 %). With digital silence between words (a test file, a noise-suppressed PC microphone)
  only loud frames are kept and it never starts: measured, the test speech file sent nothing.
  The read adds white noise of +-20 LSB (about -68 dBFS), as the Wii Speak's analog input has;
  then speech and tones start at once.
- **Echo suppression = half duplex.** While the other player's voice plays, the game's suppressor
  holds the microphone frame down and VCT's voice detection never fires (measured: the visitor
  with push-to-talk sent 0 frames while the host talked, 11+ logged sends with the host silent).
  F1 "Echo suppression" (`[online] mic_echo_suppression`, on = as on the Wii) off skips
  `fn_80229D08` (hooked: original or pass-through) for headphone users: both can talk at once.
- **Privacy**: off by default; no recording device is opened and nothing is read while off. The
  capture opens only while the game has the Wii Speak open, or while the F1 section is shown
  (level meter), and closes when the microphone is turned off. With push-to-talk, samples read
  while the key is up are dropped (no frame reaches VCT, nothing is sent). Voice goes only to the
  players of the visit, through the room's sealed connection.

**F1 > Online > Voice chat (Wii Speak)**: Microphone (off), Device (system default or a named SDL
recording device), Push-to-talk / Open mic, the push-to-talk key (default V; "Change..." takes the
next key or a button of port 1's controller), Input gain (0-4), Echo suppression, a level meter
(dBFS, "clipping") with "sending" while frames go to the game. Settings `[online] mic_on,
mic_device, mic_push_to_talk, mic_ptt_key (SDL scancode), mic_ptt_pad_button, mic_gain,
mic_echo_suppression` (own settings, not synced while visiting).

**Test aids**: `CF_MIC_WAV=<16-bit or float WAV>` replaces the microphone (forced on, looped,
resampled to 8 kHz; open mic unless `CF_MIC_PTT_CYCLE=<on s>,<off s>` holds push-to-talk in that
rhythm), `CF_MIC_LOG=1` (`[voice]` PMIC calls, talking on/off, every 5 s: reads, frames given to
the game, held back, short reads, peak), `CF_MIC_ECHO=0|1` overrides Echo suppression,
`CF_ONLINE_LOG=1` now also logs `SendUnreliable` and unreliable receives (`_VCT...`) apart from
the reliable ones. Analysis: scratch `voicecheck.py` correlated the band-passed loudness envelope
of the listener's recorded game output (`CF_AUDIO_WAV_RAW`) with the looped test speech.
Routes (scratch, from online-1's gate scripts): Copper's menu shows at a load-dependent frame
and a scripted `@x/y` pointer drifts off its line after a while, so the host route hovers
"Invite guests!" and presses A every 100 frames; the visitor's Copper talk is anchored with
`after link d_a_npc_sp_gatekeeper_aNP`. The host must not press A on after the gate is open
(it reopens Copper's menu and closes the gate), and must not still be talking to Copper when the
visitor asks to come ("You cannot enter because there seems to be a conversation in progress").
Test speech: Windows' own TTS (System.Speech) written to a 16 kHz WAV.

**Verified 2026-11-03** (two instances on this PC, real visit through Copper, host speaks a TTS
test sentence via CF_MIC_WAV, visitor without microphone): both games PMICInit when Copper
connects; the host's game opens/starts the Wii Speak once the visitor is in the visit, reads 7.0
frames/s; the host sends `_VCT` 912-byte packets (~7/s; 1000+ in 2 minutes), the visitor
receives every one; the visitor's recorded game output matches the test speech (envelope
correlation 0.84-0.88 in every 20 s window after the stream started, 0.14-0.26 before it); the
visitor (mic on, push-to-talk cycling 4 s / 4 s, host silent) sends only while "talking" (frames
given 777, held back 772). Full duplex (`CF_MIC_ECHO=0` on both, host speech open mic, visitor
tones with push-to-talk): both send at once (host 1000+ packets, visitor 500+ in the same two
minutes, each received by the other) and the visitor's output still matches the host's speech
(0.78-0.87). The host's HUD shows the game's grey Wii Speak icon (top right); the visitor's (mic
off) not. F1: `CF_MENU_ID_CHECK=exit` sweep with the voice section open: 0 ID conflicts.
Not tested: a real microphone, two PCs.

**Speaking mark**: none in City Folk as far as the game code and the screens show. Both players
on one screen in front of the gate (host speaking the TTS sentence, 2000+ voice packets received
by the visitor, recorded every 50 frames on both): no mark over the speaker on either screen,
the HUD's Wii Speak icon unchanged (grey, only on the player who has one). The game's VCT event
callback (`fn_801713F0`) handles only events 7 (start streaming), 9 and 12 (session over); no game
code asks VCT who is talking. What the game does show: the HUD icon, the "Now starting Mic Chat.
Please don't raise the volume while you're using Mic Chat." ticker when the conference starts
(`fn_800E50DC` -> notice 0x11), and each player's Wii Speak presence in its status data
(`fn_800E8C44` -> `fn_800E69F4`).

**Black host screen (fixed, branch online-voice, commit "Online DWC: unreliable data in its own
receive buffer")**: the host walking out of the gatehouse into town just after a visitor arrived,
with voice streaming, left the host's picture black for good (6 of ~11 such runs; the game kept
running, voice and chat too). Cause: the host's scene change asks every visitor first (visit
event 0x0F; the visit state machine at 0x805B2950 waits in state 0xB, `fn_800DB164`) and the
visitor answers with event 0x10 (`fn_800DEDCC`). The visitor's game reads each reliable
message straight from the DWC receive buffer (`DWC_SetRecvBuffer`, aid 0: 0x814219A0) a frame or
more after its receive callback; our DWC copied VCT voice packets (unreliable) into the same
buffer, so the request was overwritten by `_VCT@D...` and parsed as a visit message with lengths
0x4354 / 0x4044 (traced: `fn_800D3BF8` called with r4 = 0x4044, the 0x0F handler never ran).
Unreliable data now reaches the callback from a buffer of its own per AID (VCT reads it at once;
the game never sends unreliable data itself). After the fix: 0 black in 6 runs of the same
route, including runs where the host's state passed through 0xB and on to the town. Test aid:
`CF_ONLINE_LOG=all` (every reliable message, small ones whole; SetRecvBuffer addresses); route
scratch `host_black.txt` (host walks out late) + `vis.txt`; `CF_WATCHDOG=1 CF_PEEK=805B2950`.

## 11. Status and what is left

Done (branch online-1): the DWC-level HLE (51 named DWC functions + IOS_Open2/IOS_Close2 the USB
library needs), the room transport (tests pass), port mapping + STUN + reachability, F1 > Online,
F1 friends with keys, test aids. Two-instance result: section 9 (DWC login, buddies, hosting,
matching and game traffic work; the trip itself waits for Copper's conversation, not yet scripted).

Left:
- **Wii Speak voice chat**: done (section 10e, branch online-voice); open: a real microphone and
  two PCs, the speaking mark on screen.
- **USB keyboard** chat: done and verified in a visit (section 10b).
- **The visit itself** end to end: a gate route for both players (and a second town fixture).
- **WiiConnect24 letters** between towns: section 10. Coordinate with main's WC24 setup (KD device
  present-but-off, `/shared2/wc24` files from sysconf.cpp prepareWc24; the game's NWC24 tasks run on
  the save path) and with the distributions agent (NWC24 downloads): the mail HLE must leave the
  "WC24 off" behaviour unchanged while online is off.
  **Download side done** (branch distrib-1, docs/DISTRIBUTIONS.md; files `src/recomp/nwc24_dl.cpp`,
  `vff.cpp`, `wc24_crypto.cpp`, `distributions.cpp`): while F1 > Online > Nintendo events is on, a
  working KD (`/dev/net/kd/request`, `/dev/net/kd/time`) and a minimal NCD (ReadConfig: one wired,
  tested connection) replace main's KdDevice, SYSCONF `NET.WCFG` = 1, and the game's own
  WiiConnect24 check reports "available". For the mail HLE: register KD's mail ioctls (0x0A check,
  0x0B send, 0x0C receive, 0x0D save) with `nwc24::setMailHandler()`, take over NCD with
  `nwc24::setNcdOwner()`, and make `nwc24::wc24On()` (nwc24_dl.h) also true when mail is on; the
  VFF writer (`vff::write`, VFAT long names as the game's VF uses them) serves `wc24recv.mbx` too.
  The town's own WiiConnect24 setting (save +0x735E4, setter `fn_80177DE0`) also gates mail.
- **Friends in the game's roster** and **presence** without a room: section 10. Leads for the
  roster: the roster menu `d_menu_friendNP` fn_139_10EC builds the entry with
  DWC_CreateFriendKeyToken, checks duplicates with `fn_8017BD64` and stores it at menu+0x5A0; the
  game's friend manager is `fn_8017BAC8`-`fn_8017C480` (names/towns of the 32 entries).
- **NAT hole punching** and the **lobby / relay**: done, section 13 (room list, coordinated hole
  punching with port prediction, TURN relay). Open there: deploying the Worker (user), a default lobby
  address in the build, `wss://` and real routers untested, Linux / macOS only compiled in parts.
- Region: PAL/Japan/Korea use the same HLE by name (tables carried); the CF_ONLINE_TEST actions
  are USA-only.

## 12. Decisions for the user

- Default UDP port 29910 (and 29911 for LAN beacons): fine, or another?
- STUN servers (Google, Cloudflare) are contacted when hosting with port mapping on: acceptable,
  or should that be its own checkbox?
- Friend codes: keep the game's own registration (both type each other's code in the game's
  roster, as on the Wii; works now), or also write F1 friends into the roster automatically
  (section 10)?
- Mail without a common online time: decided 2026-11-03: the player's own email account (optional) and
  delivery whenever both games run; no DHT, no letters on the relay (section 10).
- The public lobby (section 13.7): deploy `tools/lobby_worker` to the Cloudflare account, decide
  whether to create the TURN key (check whether it asks for billing details), then give the URL so
  it can go into `kDefaultLobbies` (done 2026-11-11: `https://cf-lobby.cf-lobby.workers.dev`).
- Session friends (13.4): room members are written into the game's roster while the room lasts and
  removed afterwards. Keep it that way, or leave visitors in the roster after a visit as the Wii did?

## 13. The lobby: room list, connecting two routers, the relay (branch online-lobby)

User report (2026-11-10): two players in different US states could not connect. Host log: `port mapping
failed: no UPnP router answered ...; no NAT-PMP / PCP answer`, `STUN: public address <host address>:29910`,
second attempt `:57747` (the router's mapping is not stable), `opening the way for <guest address>:59581
(hole punching, 30 s)` repeated, never connected. Neither side had a forwarded port; the pasted-code hole
punch (10d) had no timing coordination and cannot follow a router that picks another port. User
requirement: no room / reply codes in the normal flow: "a list of all online rooms available (refreshes
every 10 seconds or manually refresh) and then you can simply click and join the room. if the room is
password protected then you enter the password." And: the lobby must be free to run and must not need
the user's PC on.

### 13.1 Parts

| Part | Where | What it does |
|---|---|---|
| Lobby (primary) | `tools/lobby_worker/` (Cloudflare Worker, free plan, one SQLite-backed Durable Object) | room directory, signaling pipe joiner <-> host, hands out relay (TURN) credentials |
| Lobby (self-hosted, tests) | `tools/lobby/cf_lobby.cpp` (one C++17 file, plain HTTP + WebSocket) | the same API; also the local stand-in |
| Public STUN servers | Google / Cloudflare (as before, 10d) | each game's public UDP address; two servers tell a symmetric router |
| Relay | Cloudflare Realtime TURN (optional), or any TURN server (coturn) with cf_lobby | carries the encrypted packets when no direct path opens |
| Client | `src/recomp/online_lobby.cpp` (HTTP/WebSocket via `src/port/http.cpp`), `online_net.cpp` (handshake, candidates, paths), `online_turn.cpp` (TURN client) | |

The room itself is unchanged (section 5): the host's game is the room, members talk to the host over UDP,
sealed end to end. The lobby only lists rooms and passes the handshake along; game data never goes
through it, and through the relay only as sealed packets.

### 13.2 Lobby API v1 (both servers)

JSON over HTTPS; `v=1` in every request (other values: HTTP 426 `version`). Errors are
`{"error":"<code>","message":"..."}`: `bad_request` 400, `no_room` 404, `timeout` 504, `too_many` 429,
`full` 503, `busy` 503, `no_turn` 404, `not_found` 404.
- `GET /v1/info?v=1` -> `{"v":1,"name":"cf-lobby","rooms":N,"turn":bool}`.
- `GET /v1/rooms?v=1[&game=RUUE01]` -> `{"v":1,"turn":bool,"rooms":[{"id","name","host","town","players",
  "max","lock","friends","game","ver","tag","age","ping"?}]}` (at most 200, newest first). Friends-only
  rooms are listed with blank `name`/`host`/`town`. `ping` (Worker only) is an estimate from the distance
  between the caller's and the host's Cloudflare locations, rounded to 10 ms; the coordinates stay in the
  Durable Object. The Worker caches the list 5 s (a new room can take 5 s to show, a closed one 5 s to go).
- `WebSocket /v1/host?v=1` (one per hosted room). Host -> lobby `{"t":"reg","room":{name, host, town,
  players, max, lock, friends, game, ver, tag}}` (again to update) -> `{"t":"reg_ok","id":"<16 hex>"}` or
  `{"t":"error","code","message"}`; `{"t":"sig","peer":"<8 hex>","data":"<base64>"}` both ways (a joiner's
  signal and its answer). Text `ping` -> `pong`. The room is listed while the socket is open; closing it
  removes the room at once; a socket silent for 120 s is closed. The client pings every 40 s.
- `POST /v1/signal {"v":1,"room","peer","data"}` (a joiner): forwarded to the host's socket; the response
  `{"data"}` is the host's answer (waits up to 8 s, else 504).
- `POST /v1/turn {"v":1,"room"}` -> `{"servers":["host:port"],"username","credential","ttl"}` or `no_turn`.
- Limits: 4 rooms and 8 host sockets per client address, 500 rooms, 30 host messages per 10 s, 20 signals
  per minute and 10 relay credentials per hour per address, 8 signals waiting per room, bodies <= 8 KB,
  WebSocket messages <= 4096 bytes, names cleaned (control characters out, 32 / 24 / 16 characters), game
  id `[A-Z0-9]{6}`. No amplification: every answer goes to the TCP/TLS connection that asked.
Every edge case is pinned down by `tools/tests/lobby_api_test.cpp` (ctest `lobby_api`, 200+ checks;
`CF_LOBBY_TEST_URL=<url>` runs it against a running Worker); both servers pass it.

### 13.3 Joining a listed room (online_net.cpp)

1. **Handshake through the lobby.** The joiner's HELLO / AUTH and the host's CHALLENGE / ACCEPT (WAIT,
   REJECT) are the same datagrams as in section 5, carried as signal blobs (two HTTPS requests on the
   joiner's side, the host's WebSocket on the other). Handshake v2 (all join kinds, wire protocol 2):
   both sides add an ephemeral X25519 key; `k = HMAC(master, "cf-k2" | X25519 shared)`, proof and session
   key = `HMAC(k, label | nonces | both ephemeral keys)`. So the lobby (or anyone on the path) cannot
   derive the session key, and cannot test password guesses offline against what it relayed. The host
   signs the transcript with its Ed25519 identity key (section 10); a joiner signs it for friends-only
   rooms. The password is checked by the host exactly as before (PBKDF2 proof); a wrong one is refused in
   ~0.3 s. Limit of this design: it is a challenge-response, not a full PAKE. A lobby that actively
   replaces the ephemeral keys (man in the middle) could collect one proof per attempt and guess a weak
   password offline from it, and could sit between two strangers in a public room. It cannot do either
   against friends: a friends-only join requires the host's key to be the friend's key
   (`expectHostKey`), and F1 shows "The host is your friend (key checked)" in any room.
2. **Candidates.** AUTH carries the joiner's UDP addresses, ACCEPT the host's, sealed under the session
   key (the lobby never sees UDP addresses or ports): LAN address, the public address two STUN servers
   reported (the host asks again every 20 s, which also keeps its router's mapping alive), the UPnP
   mapping if any, global IPv6, and for a router that showed the two STUN servers different ports
   ("symmetric") six predicted next ports (last port + k x the difference).
3. **Opening a path.** Both sides send sealed pings to every candidate of the other (every 250 ms for
   10 s, then every second); a ping is answered on the path it came in on, and a path counts once a pong
   came back over it (round trip proved, nothing forgeable: only sealed packets count). A valid packet
   from an address nobody listed (the router picked another port) becomes a candidate too. Direct paths
   win over relayed ones; a relayed link keeps probing for a direct one every 15 s; a path silent for 4 s
   is dropped and the search starts again.
4. **Relay.** After 3 s without a path (1 s when the joiner's own router is symmetric) the joiner asks the
   lobby for TURN credentials, allocates a relay address on the TURN server from its game socket
   (`online_turn.cpp`: RFC 5766/8656 over UDP, long-term credentials, Send / Data indications,
   permissions for the host's addresses), and sends that address to the host through the lobby (T_CAND,
   sealed with the link key). The host just sees one more candidate; the joiner reaches the host's
   addresses through the relay. Only the joiner needs credentials. Without a relay (lobby without TURN
   keys) the join fails after 12 s with: "Could not open a connection to the host: both routers refuse
   direct connections and this lobby has no relay. A port forward on the host's router (UDP 29910) or a
   VPN (Tailscale, ZeroTier) works".
5. F1 shows "Connection to the host: direct / relayed", and per member for the host.

Test switch `CF_ONLINE_FORCE_RELAY=1` (`net::setForceRelay`): only relay paths are used.

### 13.4 Friends for the session (no friend requests needed)

The game's gate lists only towns of roster friends who registered each other (sections 2, 4). The old
flow needed an F1 friend request + accept in the room before the roster entries were written. Now every
room member whose key is proved (IDENT / PROVE) and whose roster entry (INFO) arrived is written into the
game's roster by rosterSync as soon as the game is offline with a town loaded ("<name> is in this room:
you can visit each other at the town gate"), on both sides, so the DWC layer sees them as buddies. These
session entries are remembered (`session <pid>` in friends.txt) and taken out of the roster again once
that player is no longer in a room with us (also after a crash), unless the two became F1 friends
meanwhile. F1 friends stay as before (letters, friends-only rooms). Blocked players are never added.

### 13.5 F1 > Online

- **Rooms**: the list (lock icon, room, host / town, players, version match, ping estimate, Join). It is
  fetched when the tab is shown and every 10 s while it stays visible (never while the menu is closed),
  plus a Refresh button. Rooms heard on the local network are in the same list ("LAN"). Rooms of another
  region or port version are shown but cannot be joined (tooltip says why). Join on a locked room opens
  a password prompt. "No lobby server set" when there is none.
- **Host a room**: name, who can join (Public / Password / Friends only), players (2-4), Host room.
  Friends only: the lobby lists the room without names; friends recognise it by its tag (4 random bytes +
  HMAC(host key, them)[0..4]) and see "<friend>'s room"; the host admits only keys of its F1 friends
  (signature over the handshake).
- **In a room**: listed-on-lobby status, router type from the two STUN answers, members with ping and
  connection (direct / relayed / through the host), Kick, join requests, Close / Leave.
- **Advanced**: the lobby server address (`[online] lobby_server`; the built-in default list in
  online.cpp `kDefaultLobbies` holds the project's lobby `https://cf-lobby.cf-lobby.workers.dev`, deployed 2026-11-11), "List my room on the
  lobby", UDP port, ask before join, port mapping, LAN visibility, direct connections, own cf_relay,
  "Connect to an address", "Join with a room code"; while hosting: the room code and the reply code box.
  The old code flow (sections 5, 7, 10c, 10d) is unchanged there.
New settings: `[online] lobby_server`, `lobby_list`, `max_players`, `visibility`.

### 13.6 What the lobby and the relay see (privacy)

- The lobby: each room's name, host player name, town name, player count, lock / friends flags, game id,
  port version, tag; the IP address of every HTTPS connection (hosts, players looking at the list,
  joiners), as any web server does. Friends-only rooms: no names. The Worker also gets Cloudflare's
  approximate location of a host (for the ping estimate), kept only with the open socket.
- It does not see: passwords or anything derived from them that could be tested offline (13.3), UDP
  addresses and ports, friend lists, keys other than the handshake's public keys, chat, towns, voice.
- Nothing is stored: a room exists only as the attachment of its host's open WebSocket (Worker) / in
  memory (cf_lobby) and is gone when the host disconnects. No logs of rooms or addresses (Worker:
  observability off; cf_lobby: a statistics line every 10 minutes, room names only with `--verbose`).
- The relay (TURN) sees the two IP addresses and sealed packets, and their sizes and timing.
- The other player: your public IP address once a direct path is used (as in any peer-to-peer game).
  With a password or friends-only room the host's addresses are only sent to joiners that passed the check.

### 13.7 Deploying the lobby on Cloudflare (free plan)

Needs Node.js 18+ on any PC (only for deploying) and a free Cloudflare account. From the repository:
```
cd tools/lobby_worker
npm install
npx wrangler login          # opens the browser: log in to the Cloudflare account, allow wrangler
npx wrangler deploy
```
- The first deploy asks once for a workers.dev subdomain if the account has none, and applies the
  Durable Object migration `v1` (`new_sqlite_classes = ["Lobby"]` in wrangler.toml: the SQLite-backed
  kind is the only one on the free plan; no further migration steps).
- The address is printed at the end: `https://cf-lobby.<your-subdomain>.workers.dev`. Check it:
  `https://cf-lobby.<your-subdomain>.workers.dev/v1/info?v=1` -> `{"v":1,"name":"cf-lobby","rooms":0,"turn":false}`.
- **Relay (optional)**: Cloudflare dashboard > Realtime > TURN Server > Create: it shows a "Turn Token ID"
  and an "API Token" once. Then
  ```
  npx wrangler secret put TURN_KEY_ID          # paste the Turn Token ID
  npx wrangler secret put TURN_KEY_API_TOKEN   # paste the API Token
  ```
  (no redeploy needed; `/v1/info` then says `"turn":true`). Cloudflare's TURN has a free allowance
  (1,000 GB per month, shared with their SFU, at the time of writing): **check at sign-up whether
  creating the TURN key asks for billing details**, and what happens above the allowance. Without the
  two secrets the lobby works Dolphin-style (hole punching only) and says so in the list ("no relay").
- Put the URL into the port: F1 > Online > Advanced > Lobby server for one PC, or into `kDefaultLobbies`
  (src/recomp/online.cpp) for everyone, then rebuild / release.
- Never commit secrets; `tools/lobby_worker/.dev.vars` (local test secrets) and `node_modules` are
  gitignored.

**Free plan budget** (100,000 requests per day shared by the Worker and its Durable Object, 10 ms CPU
per request):

| Who | Requests |
|---|---|
| A host, per hosting session (any length) | 2 at connect + 1/20 per message (registration, player count changes, signal answers): about 3 |
| A player with the room list open | 1 Worker request per refresh = 6 per minute; the Durable Object is asked at most once per 5 s per Cloudflare location, however many players look |
| A join | about 4 (two signals, each 1 Worker + 1 Durable Object request), +2 when it needs the relay, +2 for the relay address |
| Keep-alive pings | answered by the edge without waking the object (if billed at all: 1/20 request each, ~108 per room per day) |

So the list is what counts: 100,000 / 6 = about 16,000 player-minutes of an open room list per day
(e.g. 500 players looking for 30 minutes each), which is why the client refreshes only while the list is
visible. Over the limit Cloudflare answers HTTP 429 (error 1027) and the game shows "The lobby is busy
(its free daily limit may be used up). Try again later"; rooms already joined are not affected (the
lobby is not in their path). The limit resets at 00:00 UTC.

Local run of the Worker (tests): `npm run dev` in tools/lobby_worker (wrangler dev, local mode, port
8787; set `WRANGLER_SEND_METRICS=false`), then `CF_LOBBY_TEST_URL=http://127.0.0.1:8787
cf_lobby_api_test`; the TURN path against `test/fake_turn.mjs` with `.dev.vars` (`TURN_KEY_ID`,
`TURN_KEY_API_TOKEN`, `TURN_API_BASE=http://127.0.0.1:8788`).

### 13.8 Self-hosting instead (cf_lobby)

For a group that prefers its own server (a small Linux VPS; any always-on machine with a public address):
```
g++ -O2 -std=c++17 -pthread tools/lobby/cf_lobby.cpp -o cf_lobby
./cf_lobby --port 29920 [--turn <host:3478> --turn-user <u> --turn-pass <p>] [--trust-proxy] [--verbose]
```
- TCP port 29920 (HTTP + WebSocket, no TLS). Players enter `http://<server>:29920` as the lobby server.
  For https put a reverse proxy that passes WebSocket upgrades in front (Caddy: `reverse_proxy
  127.0.0.1:29920`) and add `--trust-proxy` (the client address then comes from `X-Forwarded-For`).
- Relay: any TURN server; coturn e.g. `turnserver --lt-cred-mech --user cf:<password> --realm cf
  --listening-port 3478 --no-tls --no-dtls --no-tcp` (UDP 3478 + its relay port range open), then
  `--turn <server>:3478 --turn-user cf --turn-pass <password>`. This is one shared static credential that
  every joiner who needs the relay receives: use it only for a group you trust, or leave the relay out.
- systemd unit (`/etc/systemd/system/cf-lobby.service`):
  ```
  [Unit]
  Description=City Folk Recompiled lobby
  After=network-online.target
  [Service]
  ExecStart=/opt/cf-lobby/cf_lobby --port 29920
  DynamicUser=yes
  Restart=always
  NoNewPrivileges=yes
  ProtectSystem=strict
  [Install]
  WantedBy=multi-user.target
  ```
  `systemctl enable --now cf-lobby`; firewall: allow TCP 29920.
- Size: one thread, a few MB of memory; 500 rooms with a few hundred players watching the list is a few
  KB/s (a list of 100 rooms is ~15 KB per refresh). The lobby carries no game traffic. A relay is the
  only bandwidth cost: a relayed visit moves ~500 KB when it starts (the town) and then 1-10 KB/s per
  player (more with voice chat, ~7 KB/s per speaker and listener); a 1 TB/month VPS covers thousands of
  relayed hours. coturn's own quotas (`--max-bps`, `--user-quota`) cap it.

### 13.9 Tests (no external service; 2026-11-10)

- `cf_turn_test` (ctest `online_turn`): the TURN client against the in-process stand-in
  (tools/tests/turn_standin.*), RFC 5769 vectors, wrong credentials, permissions, refresh, lost
  requests, garbage datagrams.
- `cf_lobby_api_test` (ctest `lobby_api`): the API against cf_lobby in-process, and with
  `CF_LOBBY_TEST_URL` against the real Worker under `wrangler dev` (wrangler 4.160, local mode): 0
  failures in both; verified locally on the Workers runtime: the WebSocket upgrade forwarded to the
  Durable Object, attachments surviving hibernation, `ping` answered without waking the object, signal
  waits resolved from the socket handler and the 8 s timeout, abrupt host disconnects, the 5 s list
  cache, the TURN credential conversion against a fake API. Only checkable on Cloudflare itself: the
  edge cache on workers.dev, real request accounting, the real TURN API answer, `wss://` end to end.
- `cf_online_lobby_test` (ctest `online_lobby`, ~100 s): the whole join path in one process, real lobby
  client + cf_lobby + TURN/STUN stand-ins, each session behind a simulated router (tools/tests/nat_sim.h:
  full cone, port-restricted cone, symmetric with sequential or random ports; every UDP datagram goes
  through it). Results (time to Joined, test timers):

| Joiner / host router | Result | Path | Time |
|---|---|---|---|
| none / none | joined | direct | 0.39 s |
| full cone / full cone | joined | direct | 0.39 s |
| port-restricted / port-restricted | joined | direct | 0.40 s |
| symmetric (sequential ports) / port-restricted | joined | direct (prediction) | 0.67 s |
| port-restricted / symmetric (sequential) | joined | direct (prediction) | 0.92 s |
| symmetric (random ports) / port-restricted | joined | relayed | 1.4 s (3.5 s before the 1 s rule for symmetric joiners) |
| symmetric (random) / symmetric (random) | joined | relayed | 1.4 s |
| full cone / full cone, forced relay | joined | relayed | 0.39 s |
| symmetric both, lobby without relay | fails with the message of 13.3 | - | 12.2 s |
| three joiners at once (full cone, symmetric sequential, symmetric random) into a port-restricted host | all joined | direct, direct, relayed | 0.4 / 0.7 / 3.3 s |
| password room: wrong / right password | refused / joined | - / direct | 0.25 s / 0.47 s |
| 2-player room, second joiner | "The room is full" | - | 0.08 s |
| friends-only: friend / stranger's key / wrong expected host key | joined / refused / refused | direct | 0.39 / 0.17 / 0.10 s |
| ask before join: yes / no | joined / refused | direct | 1.9 / 3.0 s |

  Every connected case also sends 50,000 bytes reliably each way, checks both member lists and the
  relayed flags, and the relay's byte counter (zero when direct). Also: a joiner's router restarting
  (new port: the link moves, stays joined), a member / the host vanishing (noticed in ~5 s with test
  timers, 15 s in play), lost CHALLENGE / ACCEPT, host closes -> room off the list, garbage / replayed
  signals and 600 random datagrams (no crash, no wrong accept, next join works). Sanitizers (ASan/UBSan,
  TSan) clean.
- Known limits found there: a symmetric *host's* port prediction is based on two fixed mappings and goes
  stale after a few joins or on a busy line (then the relay takes over); symmetric on both sides always
  needs the relay; relay -> direct takes up to 15 s (probe interval) and the TURN allocation is kept;
  wrong passwords through the lobby never lock a room (user decision 2026-11-11: the old shared counter let
  anyone lock a password room; only a directly connecting address is held off for 60 s after 5 wrong
  tries); a kicked lobby member is
  banned by profile id only; a lost T_CAND is not resent (the join fails and is tried again by hand);
  two players behind one router rely on the LAN candidate (not simulated); IPv6 not simulated.
- **Two game instances on this PC** (dev build, copies of the test_builds/online-letter-test-2
  profiles, local cf_lobby + two `cf_turn_standin` as relay and STUN servers via `CF_ONLINE_STUN`, so
  nothing leaves the PC; hosts with `lan_visible=0`, else a same-PC joiner takes the LAN row):

| Test | Result | Evidence |
|---|---|---|
| Host a room, it shows in the other game's list, click Join (`CF_MENU_CLICK ... Join##r0`), leave | pass | `[lobby] room listed on ...`, `Bob joined as member 1 through the lobby`, `connected, path to member 1: ... (direct)`, `member 1 (Bob) left: left` |
| Password room: wrong / right password (the prompt's `Join room`) | pass | `join refused: Wrong password`, shown in F1; right one `Joined "Locked room"` |
| Friends-only room: an F1 friend / a third install with another key | pass | friend sees "rob64's room", joins, "The host is your friend (key checked)"; the stranger's list stays empty |
| Forced relay (`CF_ONLINE_FORCE_RELAY=1`) | pass | `relay: ready at 127.0.0.9:...`, `(relayed)` on both sides, F1 "Connection to the host: relayed", the stand-in's byte counter grows |
| Host quits / is killed | pass | `/v1/rooms` empty 26 ms / 233 ms later; gone from the other game's list at its next refresh |
| Lobby down / none set | pass | "Could not reach the lobby (...)", nothing hangs; "No lobby server set", hosting still works (LAN / code) |
| List refresh only while visible | pass | lobby request counter 3 -> 6 in 40 s with the tab open, flat with the menu closed |
| A complete visit from the room list (visit_host2 / visit_guest2 routes, `CF_ONLINE_JOIN=lobby`) | pass | `ConnectToGameServer: visiting friend 0`, matched, town data, both players on both screens, visitor home again |
| The same visit with the forced relay | pass | 1,149,936 bytes through the relay; the 478 KB town transfer took 0.10 s (0.08 s direct; loopback, so only "no stall") |
| Session friends: two installs that are not F1 friends (new keys) | pass | `friend tester (profile id ...) -> game roster slot 1, as tester of test: registered`, `tester is in this room: you can visit each other at the town gate` on both sides, visit completes |
| Session entries removed after the room (leave in town, or next start) | pass | `profile id ... removed from the game roster (slot 1): done`, no `session` line left in friends.txt |
| F1 ID sweep (`CF_MENU_ID_CHECK=exit`: list with 7 rows, hosting, joined) | pass | 0 conflicts, three runs |

  Not seen on screen: Copper's town list itself (shots start after it). 4K shots of the page (room list
  with locked / full / other-region / other-version rooms, password prompt, hosting and joined pages
  direct and relayed, Advanced, no lobby set): `out/online_lobby/` of the branch's worktree (window
  capture of a `--window 3840x2160` run; CF_SHOT dumps are taken before the UI). Oddities: a relayed
  joiner logs "joined" before "connected ... (relayed)"; a room hosted at the title has no town name
  until a town is loaded; a pending CF_MENU_CLICK opens every collapsing header (test aid only).
- **Not tested**: two real routers / two PCs, `wss://` to a deployed Worker, the real Cloudflare TURN
  service, Linux / macOS clients (http.cpp's libcurl WebSocket ran on Linux loopback only), PAL / JP / KR.
