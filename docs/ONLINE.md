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
  INFO 0x45): +0x08 player (u16 id, 9 UTF-16 units, kind 0x0A) = its save+0x7ED8, +0x1E town
  (kind 0x02) = save+0x7EC2, +0x36 0x42 bytes = save+0x10C8 (the fields fn_800AC160 builds the
  DWC status data from), names terminated and kinds forced on receipt. Registration waits for
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

City Folk takes chat text from a USB keyboard (its KBD library over IOS `/dev/usb/kbd`). With
F1 > Online "USB keyboard" (`[online] usb_keyboard`, off by default) the device opens and the PC
keyboard is that keyboard: the module's async ioctl 0 gets 16-byte messages (public IOS
interface): `{u32 type: 0 attached, 1 detached, 2 keys; u32 keyboard id; u8 modifiers (HID bits);
u8 0; u8 keys[6] (HID usages)}`. The host thread polls the SDL keyboard state each frame
(`ios::usbKeyboardPoll`, SDL scancodes are HID usages) and queues a message on every change.
Off: as before (the open fails: no keyboard). Keys bound to the Wii Remote act on both, so play
with a controller while typing.

## 11. Status and what is left

Done (branch online-1): the DWC-level HLE (51 named DWC functions + IOS_Open2/IOS_Close2 the USB
library needs), the room transport (tests pass), port mapping + STUN + reachability, F1 > Online,
F1 friends with keys, test aids. Two-instance result: section 9 (DWC login, buddies, hosting,
matching and game traffic work; the trip itself waits for Copper's conversation, not yet scripted).

Left:
- **Wii Speak voice chat** with the PC microphone: PMIC / the USB device under it
  (`/dev/usb/oh0`) are absent, so VCT never records; the transport already carries VCT packets
  (unreliable, the game hands them to VCT). Needs a PMIC-level HLE (SDL3 audio capture, opt-in,
  push-to-talk) and testing the VCT streaming between two instances.
- **USB keyboard** for chat (KBD library on `/dev/usb/kbd`): host keyboard events.
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
- Mail without a common online time: decided 2026-11-03: the player's own email account (optional) and
  delivery whenever both games run; no DHT, no letters on the relay (section 10).
- Who hosts an optional public relay/lobby, if ever (none needed for rooms).
