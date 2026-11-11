// A local stand-in for Cloudflare's TURN credentials API, for testing the Worker's POST /v1/turn without
// calling the real one. Loopback only, no dependencies:
//   node test/fake_turn.mjs [port, default 8788]
// and start the Worker with these in tools/lobby_worker/.dev.vars (never committed):
//   TURN_KEY_ID="test-key-id"
//   TURN_KEY_API_TOKEN="test-token"
//   TURN_API_BASE="http://127.0.0.1:8788"
// Then run the API test with CF_LOBBY_TEST_URL=http://127.0.0.1:8787 CF_LOBBY_TEST_TURN_FAKE=http://127.0.0.1:8788
//
//   POST /v1/turn/keys/<id>/credentials/generate-ice-servers   the answer of the current mode
//   POST /mode/<array|object|error|empty>                      array: the documented answer (a list of ICE servers);
//                                                              object: the older shape; error: HTTP 500; empty: no servers
//   GET  /last                                                 the last credentials request seen (path, authorization, body, count)
import http from "node:http";

const port = Number(process.argv[2] || 8788);
let mode = "array";
let last = { path: "", authorization: "", contentType: "", body: "", count: 0 };

const turnUrls = [
  "turn:turn.cloudflare.com:3478?transport=udp",
  "turn:turn.cloudflare.com:3478?transport=tcp",
  "turns:turn.cloudflare.com:5349?transport=tcp",
  "turn:turn.cloudflare.com:53?transport=udp",
  "turn:turn.cloudflare.com:80?transport=tcp",
  "turns:turn.cloudflare.com:443?transport=tcp",
];

function answer(res, status, value) {
  const body = JSON.stringify(value);
  res.writeHead(status, { "Content-Type": "application/json", "Content-Length": Buffer.byteLength(body) });
  res.end(body);
}

http
  .createServer((req, res) => {
    let body = "";
    req.on("data", (chunk) => {
      if (body.length < 65536) body += chunk;
    });
    req.on("end", () => {
      const path = req.url || "";
      if (req.method === "GET" && path === "/last") return answer(res, 200, last);
      const m = /^\/mode\/(array|object|error|empty)$/.exec(path);
      if (req.method === "POST" && m) {
        mode = m[1];
        return answer(res, 200, { mode });
      }
      if (req.method === "POST" && /^\/v1\/turn\/keys\/[^/]+\/credentials\/generate-ice-servers$/.test(path)) {
        last = {
          path,
          authorization: req.headers.authorization || "",
          contentType: req.headers["content-type"] || "",
          body,
          count: last.count + 1,
        };
        if (mode === "error") return answer(res, 500, { error: "fake failure" });
        if (mode === "empty") return answer(res, 201, { iceServers: [{ urls: ["stun:stun.cloudflare.com:3478"] }] });
        if (mode === "object") {
          return answer(res, 201, { iceServers: { urls: ["turn:relay.example:3479?transport=udp", "turn:relay.example"], username: "old-user", credential: "old-pass" } });
        }
        return answer(res, 201, {
          iceServers: [
            { urls: ["stun:stun.cloudflare.com:3478", "stun:stun.cloudflare.com:53"] },
            { urls: turnUrls, username: "fake-user", credential: "fake-pass" },
          ],
        });
      }
      answer(res, 404, { error: "not_found" });
    });
  })
  .listen(port, "127.0.0.1", () => {
    console.log("fake TURN API on http://127.0.0.1:" + port);
  });
