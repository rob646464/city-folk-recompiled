// cf-lobby: the online lobby of City Folk Recompiled as a Cloudflare Worker (free plan) with one
// SQLite-backed Durable Object, "Lobby" (a single instance named "global"), that holds the hosts'
// WebSockets with the WebSocket Hibernation API. It implements the lobby HTTP API v1 (the same API as the
// self-hosted tools/lobby/cf_lobby.cpp): a room directory and a signaling pipe between a joining player
// and a room's host. It never carries game data; signal blobs are opaque to it.
//
// What it stores: nothing durable. No Durable Object storage, no KV, no D1, no logs.
// - A room lives in its host socket's attachment (serializeAttachment: the room's listed fields, the
//   host's Cloudflare location for the ping estimate, the client address for the per-address limits,
//   two timestamps). It is gone the moment the socket closes.
// - The client address is also a socket tag (to count one address's sockets without reading every
//   attachment). Attachments and tags exist only with the live socket; they are never sent out.
// - Rate-limit counters and waiting signals are plain memory of the Durable Object. They reset whenever
//   the object hibernates or is moved (it hibernates after ~10 s without events); that only makes the
//   limits a little more generous, and a waiting signal keeps the object awake until it is answered.
// - The Worker keeps the raw room list for 5 s (its own memory and caches.default, under an internal
//   key no request can reach), so list requests rarely reach the Durable Object.
//
// Free plan budget (100,000 requests a day, Worker and Durable Object requests together; 10 ms CPU per
// request). Nothing here polls, no alarm fires, no request writes storage:
// - a host session: 1 Worker request + 1 Durable Object request (the WebSocket upgrade), then 1/20
//   request for every message the host sends (incoming WebSocket messages are billed 20:1): its
//   registration, each room update and each signal answer. The keep-alive "ping" every 40 s is answered
//   by the edge (setWebSocketAutoResponse) without waking the object.
// - a player browsing the list: 1 Worker request per refresh; the Durable Object is asked at most once
//   per 5 s per Cloudflare location, however many players are looking.
// - a join: 1 Worker + 1 Durable Object request per signal (+ 1/20 for the host's answer); relay
//   credentials (POST /v1/turn, only after a failed hole punch): 1 Worker + 1 Durable Object request
//   and one call to Cloudflare's TURN API.
//
// Secrets (optional, for POST /v1/turn): TURN_KEY_ID and TURN_KEY_API_TOKEN of a Cloudflare Realtime
// TURN key. Without them /v1/turn answers no_turn and the lists say "turn": false. The optional variable
// TURN_API_BASE replaces https://rtc.live.cloudflare.com in local tests (test/fake_turn.mjs).
//
// Local run (no Cloudflare account, nothing leaves this PC): npm install, then npm run dev; the API test
// runs against it with CF_LOBBY_TEST_URL=http://127.0.0.1:8787 (tools/tests/lobby_api_test.cpp).
import { DurableObject } from "cloudflare:workers";

const VERSION_MESSAGE = "This lobby needs a newer City Folk Recompiled";
const MAX_BODY = 8192; // request bodies over 8 KB -> 400
const LIST_TTL_MS = 5000; // the Worker's copy of the room list
const MAX_LISTED = 200;
const MAX_ROOMS = 500;
const MAX_SOCKETS = 600; // a little above the room limit: a host whose old socket is not gone yet can reconnect
const ROOMS_PER_IP = 4;
const SOCKETS_PER_IP = 8;
const STALE_MS = 120000; // a socket that sent nothing (not even "ping") for this long is closed
const SIGNAL_WAIT_MS = 8000;
const MAX_WAITING_PER_ROOM = 8;
const SIGNALS_PER_MINUTE = 20;
const TURNS_PER_HOUR = 10;
const MESSAGES_PER_10S = 30;
const MAX_MESSAGE_BYTES = 4096;
const TURN_TTL = 21600;

const STATUS = {
  version: 426,
  bad_request: 400,
  no_room: 404,
  timeout: 504,
  too_many: 429,
  full: 503,
  busy: 503,
  no_turn: 404,
  not_found: 404,
};

const HEX16 = /^[0-9a-fA-F]{16}$/;
const HEX8 = /^[0-9a-fA-F]{8}$/;
const SIGNAL_DATA = /^[A-Za-z0-9+/=_-]{0,2800}$/; // base64 (either alphabet); opaque otherwise
const GAME_ID = /^[A-Z0-9]{6}$/;
const VERSION_TEXT = /^[A-Za-z0-9._+-]{0,24}$/;
const CONTROL_CHARS = /[\u0000-\u001f\u007f]/g;
const ENCODER = new TextEncoder();

function json(status, value, headers) {
  return new Response(JSON.stringify(value), {
    status,
    headers: { "Content-Type": "application/json", "Cache-Control": "no-store", ...headers },
  });
}

function fail(code, message) {
  return json(STATUS[code], { error: code, message });
}

function isObject(v) {
  return v !== null && typeof v === "object" && !Array.isArray(v);
}

// ---- the Worker ------------------------------------------------------------------------------------

export default {
  async fetch(request, env, ctx) {
    try {
      return await handle(request, env, ctx);
    } catch (e) {
      // The Durable Object is over its daily quota, overloaded or restarting.
      return fail("busy", "The lobby is busy, try again later");
    }
  },
};

function lobby(env) {
  return env.LOBBY.get(env.LOBBY.idFromName("global"));
}

function hasTurn(env) {
  return Boolean(env.TURN_KEY_ID && env.TURN_KEY_API_TOKEN);
}

async function handle(request, env, ctx) {
  const url = new URL(request.url);
  const path = url.pathname;
  const isGetPath = path === "/v1/info" || path === "/v1/rooms" || path === "/v1/host";
  const isPostPath = path === "/v1/signal" || path === "/v1/turn";
  if (!isGetPath && !isPostPath) return fail("not_found", "No such path");
  if (isGetPath && request.method !== "GET") return fail("bad_request", "Use GET for this path");
  if (isPostPath && request.method !== "POST") return fail("bad_request", "Use POST for this path");
  // The client address, for the limits only: passed to the Durable Object, never stored or logged.
  const ip = (request.headers.get("CF-Connecting-IP") || "unknown").slice(0, 64);

  if (isGetPath) {
    if (url.searchParams.get("v") !== "1") return fail("version", VERSION_MESSAGE);
    if (path === "/v1/info") {
      const list = await roomList(request, env, ctx);
      return json(200, { v: 1, name: "cf-lobby", rooms: list.rooms.length, turn: hasTurn(env) });
    }
    if (path === "/v1/rooms") return rooms(request, env, ctx, url);
    return hostSocket(request, env, ip);
  }

  const body = await readJson(request);
  if (!isObject(body)) return fail("bad_request", "The body is not a JSON object");
  if (body.v !== 1) return fail("version", VERSION_MESSAGE);
  if (typeof body.room !== "string" || !HEX16.test(body.room)) return fail("bad_request", "Bad room id");
  const room = body.room.toLowerCase();

  if (path === "/v1/signal") {
    if (typeof body.peer !== "string" || !HEX8.test(body.peer)) return fail("bad_request", "Bad peer id");
    if (typeof body.data !== "string" || !SIGNAL_DATA.test(body.data)) return fail("bad_request", "Bad signal data");
    // The Durable Object answers with the host's answer (or no_room / timeout / too_many).
    return lobby(env).fetch("https://lobby/signal", {
      method: "POST",
      body: JSON.stringify({ room, peer: body.peer, data: body.data, ip }),
    });
  }

  // /v1/turn
  if (!hasTurn(env)) return fail("no_turn", "This lobby has no relay");
  const check = await lobby(env).fetch("https://lobby/turn", { method: "POST", body: JSON.stringify({ room, ip }) });
  if (check.status !== 200) return check; // too_many, no_room
  return turnCredentials(env);
}

// The request body as parsed JSON; undefined when it is too large, not UTF-8 or not JSON.
async function readJson(request) {
  const declared = request.headers.get("Content-Length");
  if (declared !== null && !(Number(declared) <= MAX_BODY)) return undefined;
  if (!request.body) return undefined;
  const reader = request.body.getReader();
  const chunks = [];
  let total = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    total += value.byteLength;
    if (total > MAX_BODY) {
      try {
        await reader.cancel();
      } catch (e) {
        // nothing to do
      }
      return undefined;
    }
    chunks.push(value);
  }
  const bytes = new Uint8Array(total);
  let at = 0;
  for (const chunk of chunks) {
    bytes.set(chunk, at);
    at += chunk.byteLength;
  }
  try {
    return JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(bytes));
  } catch (e) {
    return undefined;
  }
}

// The raw room list {at, rooms:[...]} (with the hosts' coordinates: never sent out as it is), at most
// LIST_TTL_MS old: from this isolate's memory, else caches.default, else the Durable Object.
let listMemo = null;

function listFresh(list, now) {
  return isObject(list) && Array.isArray(list.rooms) && Math.abs(now - list.at) < LIST_TTL_MS;
}

async function roomList(request, env, ctx) {
  const now = Date.now();
  if (listFresh(listMemo, now)) return listMemo;
  // An internal key: a request for this URL from outside is answered by handle() (404), never by the cache.
  const cacheKey = new Request(new URL("/__lobby/raw-rooms-v1", request.url).toString());
  let cache = null;
  try {
    cache = caches.default;
    const hit = await cache.match(cacheKey);
    if (hit) {
      const list = await hit.json();
      if (listFresh(list, now)) {
        listMemo = list;
        return list;
      }
    }
  } catch (e) {
    cache = null; // no usable cache here (some previews): the Durable Object answers
  }
  const answer = await lobby(env).fetch("https://lobby/rooms");
  if (answer.status !== 200) throw new Error("lobby " + answer.status);
  const list = await answer.json();
  list.at = Date.now();
  listMemo = list;
  if (cache) {
    const copy = new Response(JSON.stringify(list), {
      headers: { "Content-Type": "application/json", "Cache-Control": "public, max-age=5" },
    });
    ctx.waitUntil(cache.put(cacheKey, copy).catch(() => {}));
  }
  return list;
}

function coordinate(v) {
  const n = typeof v === "number" ? v : typeof v === "string" && v !== "" ? Number(v) : NaN;
  return Number.isFinite(n) ? n : null;
}

// A ping estimate from the great-circle distance between two Cloudflare locations, in steps of 10 ms.
function pingEstimate(lat1, lon1, lat2, lon2) {
  if (lat1 === null || lon1 === null || lat2 === null || lon2 === null) return null;
  const rad = Math.PI / 180;
  const a =
    Math.sin(((lat2 - lat1) * rad) / 2) ** 2 +
    Math.cos(lat1 * rad) * Math.cos(lat2 * rad) * Math.sin(((lon2 - lon1) * rad) / 2) ** 2;
  const km = 6371 * 2 * Math.asin(Math.min(1, Math.sqrt(a)));
  return Math.round((20 + km / 50) / 10) * 10;
}

async function rooms(request, env, ctx, url) {
  const game = url.searchParams.get("game");
  if (game !== null && !GAME_ID.test(game)) return fail("bad_request", "Bad game id");
  const list = await roomList(request, env, ctx);
  const now = Date.now();
  const cf = request.cf || {};
  const lat = coordinate(cf.latitude);
  const lon = coordinate(cf.longitude);
  const out = [];
  for (const r of list.rooms) {
    // newest first already
    if (game !== null && r.game !== game) continue;
    const room = {
      id: r.id,
      name: r.name,
      host: r.host,
      town: r.town,
      players: r.players,
      max: r.max,
      lock: r.lock,
      friends: r.friends,
      game: r.game,
      ver: r.ver,
      tag: r.tag,
      age: Math.max(0, Math.floor((now - r.t) / 1000)),
    };
    const ping = pingEstimate(lat, lon, coordinate(r.lat), coordinate(r.lon));
    if (ping !== null) room.ping = ping;
    out.push(room);
    if (out.length >= MAX_LISTED) break;
  }
  return json(200, { v: 1, turn: hasTurn(env), rooms: out }, { "Cache-Control": "public, max-age=5" });
}

function hostSocket(request, env, ip) {
  if ((request.headers.get("Upgrade") || "").toLowerCase() !== "websocket") {
    return fail("bad_request", "This path is a WebSocket");
  }
  const cf = request.cf || {};
  const to = new URL("https://lobby/host");
  to.searchParams.set("ip", ip);
  const lat = coordinate(cf.latitude);
  const lon = coordinate(cf.longitude);
  if (lat !== null && lon !== null) {
    to.searchParams.set("lat", String(lat));
    to.searchParams.set("lon", String(lon));
  }
  // The upgrade request itself goes on to the Durable Object; its answer (101 with the socket, or an
  // error) goes back as it is.
  return lobby(env).fetch(new Request(to.toString(), request));
}

// "turn:host:port?transport=udp" -> "host:port" (UDP only; default port 3478); null for anything else.
function turnHostPort(u) {
  if (typeof u !== "string" || !/^turn:/i.test(u)) return null;
  let rest = u.slice(5);
  const q = rest.indexOf("?");
  const query = q < 0 ? "" : rest.slice(q + 1).toLowerCase();
  if (q >= 0) rest = rest.slice(0, q);
  if (/(^|&)transport=tcp($|&)/.test(query) || rest === "") return null;
  if (!/:\d+$/.test(rest)) rest += ":3478";
  return rest;
}

async function turnCredentials(env) {
  // TURN_API_BASE is for local tests only (tools/lobby_worker/test/fake_turn.mjs); never set it in production.
  const base = String(env.TURN_API_BASE || "https://rtc.live.cloudflare.com").replace(/\/+$/, "");
  const answer = await fetch(
    base + "/v1/turn/keys/" + encodeURIComponent(env.TURN_KEY_ID) + "/credentials/generate-ice-servers",
    {
      method: "POST",
      headers: { Authorization: "Bearer " + env.TURN_KEY_API_TOKEN, "Content-Type": "application/json" },
      body: JSON.stringify({ ttl: TURN_TTL }),
    },
  );
  if (!answer.ok) return fail("busy", "The relay is not available right now");
  const j = await answer.json();
  // {iceServers:[{urls:[...]}, {urls:[...], username, credential}]}, or the older {iceServers:{urls, username, credential}}.
  let entries = isObject(j) ? j.iceServers : null;
  if (!Array.isArray(entries)) entries = isObject(entries) ? [entries] : [];
  for (const e of entries) {
    if (!isObject(e) || typeof e.username !== "string" || typeof e.credential !== "string") continue;
    const urls = Array.isArray(e.urls) ? e.urls : [e.urls];
    const servers = [];
    for (const u of urls) {
      const server = turnHostPort(u);
      if (server && !servers.includes(server)) servers.push(server);
    }
    if (servers.length) return json(200, { servers, username: e.username, credential: e.credential, ttl: TURN_TTL });
  }
  return fail("busy", "The relay is not available right now");
}

// ---- room fields -------------------------------------------------------------------------------------

// A name as the lobby lists it: control characters (< 0x20, 0x7F) removed, spaces trimmed, cut to `max`
// code points (and trimmed again). Lone surrogates become U+FFFD.
function cleanName(s, max) {
  s = typeof s.toWellFormed === "function" ? s.toWellFormed() : s.replace(/\p{Cs}/gu, String.fromCharCode(0xfffd));
  s = s.replace(CONTROL_CHARS, "").replace(/^ +| +$/g, "");
  const points = Array.from(s);
  if (points.length > max) s = points.slice(0, max).join("").replace(/ +$/, "");
  return s;
}

// The room of a "reg" message as it is kept in the attachment (short keys: attachments are small), or a
// string saying what is wrong with it.
function cleanRoom(r) {
  const names = {};
  for (const [key, max] of [["name", 32], ["host", 24], ["town", 16]]) {
    if (r[key] === undefined) names[key] = "";
    else if (typeof r[key] !== "string") return "Bad " + key;
    else names[key] = cleanName(r[key], max);
  }
  for (const key of ["lock", "friends"]) {
    if (r[key] !== undefined && typeof r[key] !== "boolean") return "Bad " + key;
  }
  if (typeof r.game !== "string" || !GAME_ID.test(r.game)) return "Bad game";
  if (typeof r.ver !== "string" || !VERSION_TEXT.test(r.ver)) return "Bad ver";
  if (typeof r.tag !== "string" || !HEX16.test(r.tag)) return "Bad tag";
  if (!Number.isInteger(r.max) || r.max < 2 || r.max > 4) return "Bad max";
  if (!Number.isInteger(r.players) || r.players < 1 || r.players > r.max) return "Bad players";
  return {
    n: names.name || "Room",
    h: names.host,
    w: names.town,
    p: r.players,
    m: r.max,
    l: r.lock === true,
    f: r.friends === true,
    g: r.game,
    v: r.ver,
    t: r.tag,
  };
}

function randomHex(bytes) {
  const b = crypto.getRandomValues(new Uint8Array(bytes));
  let s = "";
  for (const v of b) s += v.toString(16).padStart(2, "0");
  return s;
}

// ---- the Durable Object ------------------------------------------------------------------------------

export class Lobby extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    // All of this is plain memory: gone when the object hibernates (see the header comment).
    this.waiters = new Map(); // "<room id>:<peer>" -> {room, finish(result)}: joiners' requests waiting for the host
    this.waiting = new Map(); // room id -> number of waiters
    this.signalRate = new Map(); // client address -> {start, count}
    this.turnRate = new Map();
    this.messageTimes = new Map(); // socket id -> times of its last messages
    this.gone = new Set(); // ids of sockets closed since the object woke up (they may still be in getWebSockets())
    this.listMemo = null; // {at, body}
    // The keep-alive: answered by the edge, without waking this object.
    this.ctx.setWebSocketAutoResponse(new WebSocketRequestResponsePair("ping", "pong"));
  }

  async fetch(request) {
    const url = new URL(request.url);
    const now = Date.now();
    switch (url.pathname) {
      case "/host":
        return this.accept(request, url, now);
      case "/rooms":
        return new Response(this.roomsBody(now), { headers: { "Content-Type": "application/json" } });
      case "/signal":
        return this.signal(await request.json(), now);
      case "/turn":
        return this.turn(await request.json(), now);
      default:
        return fail("not_found", "No such path");
    }
  }

  // The open sockets (all, or those with a tag) with their attachments. Sockets that sent nothing for
  // STALE_MS are closed here: there is no timer, so this runs whenever somebody asks.
  sockets(now, tag) {
    const out = [];
    const all = tag === undefined ? this.ctx.getWebSockets() : this.ctx.getWebSockets(tag);
    for (const ws of all) {
      if (ws.readyState === 2 || ws.readyState === 3) continue; // closing, closed
      const att = ws.deserializeAttachment();
      if (!att) {
        this.close(ws, 1011, "no state");
        continue;
      }
      if (this.gone.has(att.id)) continue;
      let seen = att.seen;
      const auto = this.ctx.getWebSocketAutoResponseTimestamp(ws); // the last "ping" the edge answered
      if (auto) seen = Math.max(seen, auto.getTime());
      if (now - seen > STALE_MS) {
        this.forget(att.id);
        this.close(ws, 1001, "no keep-alive");
        continue;
      }
      out.push({ ws, att });
    }
    return out;
  }

  close(ws, code, reason) {
    try {
      ws.close(code, reason);
    } catch (e) {
      // closed already
    }
  }

  // A socket is going away: its room is no longer listed, joiners waiting for it are told.
  forget(id) {
    if (this.gone.size > 10000) this.gone.clear(); // an object that never sleeps: the old ones left getWebSockets() long ago
    this.gone.add(id);
    this.messageTimes.delete(id);
    this.listMemo = null;
    for (const waiter of [...this.waiters.values()]) {
      if (waiter.room === id) waiter.finish({ error: "no_room" });
    }
  }

  allow(counters, ip, limit, windowMs, now) {
    if (counters.size > 5000) {
      for (const [key, c] of counters) if (now - c.start >= windowMs) counters.delete(key);
    }
    let c = counters.get(ip);
    if (!c || now - c.start >= windowMs) {
      c = { start: now, count: 0 };
      counters.set(ip, c);
    }
    if (c.count >= limit) return false;
    c.count++;
    return true;
  }

  // GET /v1/host: the host's socket. It gets its room id now (also its tag, so a signal finds it
  // without reading every attachment); the room appears with the first "reg".
  accept(request, url, now) {
    if ((request.headers.get("Upgrade") || "").toLowerCase() !== "websocket") {
      return fail("bad_request", "This path is a WebSocket");
    }
    const ip = (url.searchParams.get("ip") || "unknown").slice(0, 64);
    if (this.ctx.getWebSockets().length >= MAX_SOCKETS && this.sockets(now).length >= MAX_SOCKETS) {
      return fail("full", "The lobby is full, try again later");
    }
    if (this.sockets(now, "ip:" + ip).length >= SOCKETS_PER_IP) {
      return fail("too_many", "Too many connections from this address");
    }
    const pair = new WebSocketPair();
    const client = pair[0];
    const server = pair[1];
    const id = randomHex(8);
    this.ctx.acceptWebSocket(server, [id, "ip:" + ip]);
    server.serializeAttachment({
      id,
      ip,
      seen: now, // the last message (the edge keeps the last "ping" itself)
      reg: 0, // when the room was registered
      lat: coordinate(url.searchParams.get("lat")),
      lon: coordinate(url.searchParams.get("lon")),
      room: null,
    });
    return new Response(null, { status: 101, webSocket: client });
  }

  // The raw list for the Worker: every room, newest first, with the hosts' coordinates.
  roomsBody(now) {
    if (this.listMemo && Math.abs(now - this.listMemo.at) < 2000) return this.listMemo.body;
    const list = [];
    for (const { att } of this.sockets(now)) {
      const r = att.room;
      if (!r) continue;
      list.push({
        id: att.id,
        // Friends-only rooms are listed without their names: friends recognise them by the tag.
        name: r.f ? "" : r.n,
        host: r.f ? "" : r.h,
        town: r.f ? "" : r.w,
        players: r.p,
        max: r.m,
        lock: r.l,
        friends: r.f,
        game: r.g,
        ver: r.v,
        tag: r.t,
        t: att.reg,
        lat: att.lat,
        lon: att.lon,
      });
    }
    list.sort((a, b) => b.t - a.t);
    const body = JSON.stringify({ rooms: list.slice(0, MAX_ROOMS) });
    this.listMemo = { at: now, body };
    return body;
  }

  // The open socket hosting this room, or null.
  host(id, now) {
    for (const s of this.sockets(now, id)) {
      if (s.att.id === id && s.att.room) return s;
    }
    return null;
  }

  // POST /v1/signal: hand the joiner's signal to the host and wait for the host's answer.
  async signal(body, now) {
    const { room, peer, data, ip } = body;
    if (!this.allow(this.signalRate, ip, SIGNALS_PER_MINUTE, 60000, now)) {
      return fail("too_many", "Too many join attempts, wait a minute");
    }
    const host = this.host(room, now);
    if (!host) return fail("no_room", "This room is no longer open");
    const key = room + ":" + peer.toLowerCase();
    // The same peer asking again replaces its earlier request (it gave up on that one).
    const earlier = this.waiters.get(key);
    if (earlier) earlier.finish({ error: "timeout" });
    if ((this.waiting.get(room) || 0) >= MAX_WAITING_PER_ROOM) {
      return fail("too_many", "This room is busy, try again in a moment");
    }
    try {
      host.ws.send(JSON.stringify({ t: "sig", peer, data }));
    } catch (e) {
      return fail("no_room", "This room is no longer open");
    }
    // A pending request keeps the object awake, so the waiter can live in memory.
    const result = await new Promise((resolve) => {
      const waiter = { room, finish: null };
      const timer = setTimeout(() => waiter.finish({ error: "timeout" }), SIGNAL_WAIT_MS);
      waiter.finish = (value) => {
        if (this.waiters.get(key) !== waiter) return;
        clearTimeout(timer);
        this.waiters.delete(key);
        const left = (this.waiting.get(room) || 1) - 1;
        if (left > 0) this.waiting.set(room, left);
        else this.waiting.delete(room);
        resolve(value);
      };
      this.waiters.set(key, waiter);
      this.waiting.set(room, (this.waiting.get(room) || 0) + 1);
    });
    if (result.error === "no_room") return fail("no_room", "This room is no longer open");
    if (result.error) return fail("timeout", "The room's host did not answer");
    return json(200, { data: result.data });
  }

  // POST /v1/turn, the part only this object can answer: the per-address limit and "the room exists".
  turn(body, now) {
    if (!this.allow(this.turnRate, body.ip, TURNS_PER_HOUR, 3600000, now)) {
      return fail("too_many", "Too many relay requests, try again later");
    }
    if (!this.host(body.room, now)) return fail("no_room", "This room is no longer open");
    return json(200, { ok: true });
  }

  sendError(ws, code, message) {
    try {
      ws.send(JSON.stringify({ t: "error", code, message }));
    } catch (e) {
      // closed meanwhile
    }
  }

  async webSocketMessage(ws, message) {
    const now = Date.now();
    const att = ws.deserializeAttachment();
    if (!att) return this.close(ws, 1011, "no state");
    const drop = (code, reason) => {
      this.forget(att.id);
      this.close(ws, code, reason);
    };
    if (typeof message !== "string") return drop(1003, "text messages only");
    if (message === "ping") return ws.send("pong"); // normally answered by the edge and never seen here
    if (
      message.length > MAX_MESSAGE_BYTES ||
      (message.length * 3 > MAX_MESSAGE_BYTES && ENCODER.encode(message).length > MAX_MESSAGE_BYTES)
    ) {
      return drop(1009, "message too large");
    }
    // 30 messages per 10 s per socket.
    const times = (this.messageTimes.get(att.id) || []).filter((t) => now - t < 10000);
    if (times.length >= MESSAGES_PER_10S) return drop(1008, "too many messages");
    times.push(now);
    this.messageTimes.set(att.id, times);

    att.seen = now;
    ws.serializeAttachment(att);
    let msg;
    try {
      msg = JSON.parse(message);
    } catch (e) {
      msg = null;
    }
    if (!isObject(msg)) return this.sendError(ws, "bad_request", "Not a JSON object");
    if (msg.t === "reg") return this.onReg(ws, att, msg, now);
    if (msg.t === "sig") return this.onSig(ws, att, msg);
    return this.sendError(ws, "bad_request", "Unknown message type");
  }

  // {"t":"reg","room":{...}}: registers this socket's room, or updates it.
  onReg(ws, att, msg, now) {
    if (!isObject(msg.room)) return this.sendError(ws, "bad_request", "No room");
    const room = cleanRoom(msg.room);
    if (typeof room === "string") return this.sendError(ws, "bad_request", room);
    if (!att.room) {
      let mine = 0;
      for (const s of this.sockets(now, "ip:" + att.ip)) if (s.att.room) mine++;
      if (mine >= ROOMS_PER_IP) return this.sendError(ws, "too_many", "Too many rooms from this address");
      let total = 0;
      for (const s of this.sockets(now)) if (s.att.room) total++;
      if (total >= MAX_ROOMS) return this.sendError(ws, "full", "The lobby is full, try again later");
      att.reg = now;
    }
    att.room = room;
    ws.serializeAttachment(att);
    this.listMemo = null;
    try {
      ws.send(JSON.stringify({ t: "reg_ok", id: att.id }));
    } catch (e) {
      // closed meanwhile
    }
  }

  // {"t":"sig","peer","data"}: the host's answer to a joiner's signal.
  onSig(ws, att, msg) {
    if (typeof msg.peer !== "string" || !HEX8.test(msg.peer)) return this.sendError(ws, "bad_request", "Bad peer id");
    if (typeof msg.data !== "string" || !SIGNAL_DATA.test(msg.data)) return this.sendError(ws, "bad_request", "Bad signal data");
    if (!att.room) return this.sendError(ws, "bad_request", "No room registered");
    const waiter = this.waiters.get(att.id + ":" + msg.peer.toLowerCase());
    if (waiter) waiter.finish({ data: msg.data }); // else the joiner gave up or timed out: dropped
  }

  async webSocketClose(ws, code, reason, wasClean) {
    const att = ws.deserializeAttachment();
    if (att) this.forget(att.id);
    this.close(ws, 1000, "bye"); // completes the closing handshake (a no-op where the runtime already did)
  }

  async webSocketError(ws, error) {
    const att = ws.deserializeAttachment();
    if (att) this.forget(att.id);
    this.close(ws, 1011, "error");
  }
}
