# Online: Nintendo WFC and WiiConnect24, then and now

How City Folk's online features worked, which revival services support them today, what a
client must do to use them, and what the PC port has to emulate. ROADMAP feature "Online play
restored".

Confidence tags: **[disc]** = read from our discs (strings in `sys/main.dol`, file names);
**[web-high/medium/low]** = from service pages, wikis and forums (untrusted data, not verified
against a running game). Nothing here was tested against a live service.

## 1. What the game has

### 1.1 Nintendo Wi-Fi Connection (DWC)

Features [web-high: manual, Nookipedia, Wikipedia]:
- **Town visits**: up to 4 players (host + 3) in one town, friends only: each side registers the
  other's 12-digit friend code at Copper's gate. Guests shop, fish, trade, write letters.
- **Chat**: text chat with the on-screen keyboard or a **USB keyboard** (Nintendo verified a
  Logitech model); emotions; **voice chat with Wii Speak** (first Wii Speak game). Everyone in
  the session hears a speaker even without their own Wii Speak (manual p. 22).
- Other online uses [web-medium]: Lloid's auction house, Blanca after a friend's visit,
  "DS Suitcase" visits are local (Download Play), not online.
- Ended May 20, 2014 [web-high].

On the disc [disc]:
- SDK: RVL DWC with GameSpy (GP presence/buddy, QR2 server, server browser, NAT negotiation,
  GT2 transport), **DWC_VCT 2.0.2** voice chat (codecs, peer streaming over the DWC connection),
  `PMIC` (Wii Speak microphone library), `KBD` (USB keyboard, `/dev/usb/kbd`), `SSL`
  (`/dev/net/ssl`), `/dev/usb/oh0` (USB host for Wii Speak).
- Matchmaking: `DWC_ConnectToFriendsAsync`, `DWC_SetupGameServer`,
  `DWC_ConnectToGameServerAsync`, `DWC_ConnectToGameServerByGroupID`, also
  `DWC_ConnectToAnybodyAsync` linked in (probably unused: visits are friend-only). Host =
  "game server" (server-client match), guests join it.
- Hosts (in all three DOLs):
  - `https://naswii.nintendowifi.net/ac` (account/login) and `/pr` (profanity check); `.dev.` and
    `.test.` variants are present for development builds.
  - `gpcm.gs.nintendowifi.net` (GameSpy presence/login), `gpsp.gs.nintendowifi.net` (search
    profile).
  - `%s.available.gs.nintendowifi.net`, `%s.master.gs.nintendowifi.net`,
    `%s.ms%d.gs.nintendowifi.net` (server browsing/QR2), `%s.natneg1/2/3.gs.nintendowifi.net`,
    `gamestats.gs.nintendowifi.net`, `gamestats2.gs.nintendowifi.net`, with `%s` =
    **`acrossingwii`** (the GameSpy game name; game id 2187 per WiiLink's server list
    [web-high]).
  - `DWC_OPTION_CONNTEST` (connection test host, string not found: it lives in the Wii's
    network settings / IOS).
- Credentials: `/shared2/DWC_AUTHDATA` (per-console WFC user id), Nintendo client certificate and
  root CA through IOS SSL (`DWC Auth: failed to set ClientCert / RootCA`), header
  `X-Wii-DWCId: %08X`. The NAS login also sends the console's MAC, serial number (from
  `/title/00000001/00000002/data/setting.txt`), the game code and the language [web-medium:
  documented for DS/3DS NAS, assumed for Wii].
- Ports [web-low: commonly cited, not seen on disc]: GPCM 29900, GPSP 29901, QR2 27900, NATNEG
  27901, master 28910, then peer-to-peer UDP.

### 1.2 WiiConnect24

Features [web-high]: letters to friends' towns (both consoles connected; slow or lost by
design), messages and photos to the Wii Message Board / phones / PCs, **Nintendo item
distributions** (letters from Nintendo carried by Pete; designs from Wendell; see
docs/REGION_CONTENT.md section 3). Turned on when Rover asks, later at the attic phone. Ended
June 27/28, 2013.

On the disc [disc]:
- NWC24 library (`<< RVL_SDK - NWC24`), files under `/shared2/wc24/`: `nwc24msg.cfg` (Wii number,
  mail domain, account/check/receive/delete/send URLs), `nwc24msg.cbk`, `nwc24fl.bin` (friend
  list), `nwc24fls.bin`, `nwc24dl.bin` (download tasks), `mbox/` (`wc24send.mbx/.ctl`,
  `wc24recv.mbx/.ctl`, `dlcnt.bin`, `recvtmp.msg`), `misc.bin`; `wc24dl.vff` (downloaded file
  store). MIME mail (`Boundary-NWC24-...`, `MAIL FROM:`).
- Download URLs: `http://nwcs.wapp.wii.com/ruu/rvforestdl_us.enc` (PAL `_eu`, JP `_jp`), the item
  distribution; `http://nwcs.wapp.wii.com/nw/f/RUD/E.enc` (`P.enc`, `J.enc`), second download,
  purpose unknown (title code RUD, maybe the profanity list `prc_list.bin` or `game_data.bin`,
  both named next to it: unverified). `wc24pubk.mod`: public key that verifies downloads.
- `https://wus.wapp.wii.com/notify` and `/inquiry` with headers `X-APPVER`, `X-GAMECD`
  (`RVL-RUDJ`), `X-RESULT`: purpose unknown (an update/notification service).
- Boot checks WiiConnect24 state (`dScBootStatic_c::Wc24CheckState_c`).

## 2. Revival services today

| Service | City Folk | Client side | Notes |
|---|---|---|---|
| **Wiimmfi** (wiimmfi.de) | Listed as supported for years (a 2017 forum post: "Full Support"; the City Folk Deluxe mod says online "mostly supported" via Wiimmfi and WiiLink) [web-medium]. Current per-region status unverified: the site is behind a bot check. | Patch `nintendowifi.net` -> `wiimmfi.de` and the NAS URL from https to http (Wii SSL cannot be used) [web-high: mkwiiki]; newer patches add more (unverified). DNS method only for some games. | Dolphin needs a NAND dumped from a real console; generated NANDs no longer accepted [web-medium: Wiimmfi's Dolphin page, seen as a search snippet]. Error 23918: too many users sharing one Dolphin identity [web-medium]. Bans cheaters including their NAND/console [web-medium]. Old GameSpy profiles carry over only if recovered (~2.9M) [web-medium]. |
| **WiiLink WFC** (wfc.wiilink.ca; Go server, AGPL-3.0) | Listed **"RUU - Partial"**, no notes on what works [web-high: WiiLink wiki list]. | Patcher (Gecko code / auto-patcher) loads a server-side, per-game payload ("stage 2", also security patches) [web-high: wfc-patcher-wii]. | Rules: no cheats/exploits, no multiple accounts for advantage; bans [web-high: wfc.wiilink.ca/rules]. Nothing about emulators or third-party clients. Privacy policy URL returned 404. Dolphin "may need" a real NAND. |
| **WiiLink City Folk distribution** (wiilink.ca/services/animalcrossing) | Serves City Folk DLC items to consoles today, run by a third party for WiiLink [web-high]; which items it sends is unknown (a fork "randomly picks" a DLC file [web-medium]). | DNS 167.235.229.36, accept the EULA, WiiConnect24 + standby on; UK region for PAL, US for NTSC-U (else error 221001). | Korean copies: unverified. |
| **WiiLink Mail** (formerly RiiConnect24) | Mail list mentions City Folk "exchanges pictures and messages", no status [web-medium]. | Mail Patcher registers via WC24 and rewrites the URLs in `nwc24msg.cfg` to WiiLink's servers [web-high]. | |
| **AltWFC** / dwc_network_server_emulator (AGPL-3.0, Python 2) | City Folk not verified. | "No SSL" option in loaders. | Small public server. |

Dolphin [web-medium]: City Folk ran on Wiimmfi in development builds (2017); Dolphin's wiki
reports a hang on save-and-quit with WiiConnect24 enabled; Dolphin emulates Wii Speak since
2025 (unverified how it works in City Folk) and has WiiLink WiiConnect24 built in since 2023.

## 3. What the PC port must emulate

Approach: keep the game's own DWC/GameSpy/VCT/NWC24 code (it is recompiled like everything
else) and HLE only the IOS devices under it. That keeps protocol behaviour exact and needs no
knowledge of GameSpy.

| Layer | Port | Notes |
|---|---|---|
| `/dev/net/ip/top` (SO sockets: socket, connect, bind, sendto/recvfrom, poll, DNS `getaddrinfo`/`gethostbyname`) | host sockets (non-blocking, modelled IPC latency) | NAT: the guest's "private address" (`SBServerGetPrivateAddress`) must be a plausible LAN address; peer UDP needs real port mapping (UPnP optional) or the NATNEG relay of the service. |
| NCD / `/dev/net/ncd/manage`, `/dev/net/kd/request` (WC24 KD), `/dev/net/kd/time` (network time), connection test | report "connected", MAC from a generated per-install value (see identity) | all device names [disc]; `/dev/net/wd/command` is DS Download Play (docs/FEATURE_RESEARCH.md, DS transfer), not internet |
| `/dev/net/ssl` | host TLS (OS store or a bundled CA list) to the revival server; or no TLS where the service uses http | The game asks IOS to load Nintendo's client cert / root CA by id: answer success, never ship Nintendo keys. |
| Hostname rewrite | patch the host strings in guest memory at load (`nintendowifi.net` -> service domain, `https://naswii` -> `http://` if the service requires it) or answer DNS for those names with the service's addresses | Same effect as the services' patchers; done in the HLE, no game binary is distributed. WiiLink's payload-loading patcher cannot run (it executes downloaded PowerPC code); WiiLink would need to accept a client without its payload (ask them). |
| `/dev/usb/kbd` + KBD | host keyboard events | also useful offline (letters) |
| Wii Speak (`/dev/usb/oh0`, PMIC) | host microphone through SDL3 audio capture, opt-in | push-to-talk option; off by default |
| NWC24 (`/shared2/wc24/*`, KD download tasks, mail send/receive) | host HTTP(S) for download tasks; mail via WiiLink Mail | Download verification with `wc24pubk.mod`: WiiLink's service already works with retail consoles, so its files pass the game's check. |
| NAND files | `nand/shared2/DWC_AUTHDATA`, `wc24/*`, `setting.txt` | per install, never the user's real console files unless they choose to import them |

Console identity: services key accounts and bans on MAC, console serial / id and (Wiimmfi, for
Dolphin) a real NAND. A port that generates identities may be rejected like Dolphin's generated
NANDs, and one shared identity would be banned (error 23918). Options: (1) per-install random
identity, if the service accepts it; (2) let the user import identity files from their own
console's NAND dump (their own data, stays local); (3) a service-side arrangement. This needs a
conversation with Wiimmfi and WiiLink before any implementation; their positions on third-party
clients are not published (unverified).

## 4. Risks

- **Service terms**: neither Wiimmfi's terms/privacy policy (not reachable) nor WiiLink's
  privacy policy (404) could be read. WiiLink's rules ban cheats/exploits; a modified client
  (the port's enhancements: frame rate, region content, debug tools) could count as a hack.
  Online mode should disable gameplay-changing options (debug warps, region-forced items?) or
  the services should be asked which are acceptable.
- **Privacy**: the NAS login sends MAC, console serial, user id, language; servers see the IP,
  friend code and every letter. The F1 option must be off by default, say what is sent and to
  whom, and identity files must stay on the user's machine.
- **Nintendo data**: never redistribute DLC files, DLC keys, Nintendo certificates or keys. The
  port only talks to services that already serve consoles.
- **Legal**: no known action by Nintendo against Wiimmfi/WiiLink (low confidence either way).
- **Security**: GameSpy/DWC code parses network data written in 2008; the recompiled guest runs
  in its own memory window, but host HLE handlers (sockets, TLS, file paths in NWC24 messages)
  must validate every length and path.

## 5. Order of work (suggested)

1. Offline completeness first: NAND files, NWC24 "disabled" path, USB keyboard.
2. WiiLink City Folk distribution (download task only; one HTTP GET per day; lowest risk; brings
   the distributed items back).
3. WiiLink Mail (letters between towns).
4. WFC visits through Wiimmfi or WiiLink WFC after the identity question is answered.
5. Wii Speak.
