// cf_turn_standin: the in-process TURN / STUN test server (turn_standin.h) as a program, for
// local runs of two game instances (docs/ONLINE.md "Testing the lobby"). Not a real TURN server:
// use coturn for that.
//   cf_turn_standin [--ip 127.0.0.9] [--port 3478] [--user cf] [--pass test]
// It also answers plain STUN binding requests, so it serves as CF_ONLINE_STUN=<ip:port> too.
#include "turn_standin.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

int main(int argc, char** argv) {
    std::string ip = "127.0.0.9", user = "cf", pass = "test";
    int port = 3478;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--ip")) ip = argv[i + 1];
        else if (!std::strcmp(argv[i], "--port")) port = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--user")) user = argv[i + 1];
        else if (!std::strcmp(argv[i], "--pass")) pass = argv[i + 1];
    }
    TurnTestServer srv;
    std::string err;
    if (!srv.start(ip, uint16_t(port), user, pass, &err)) {
        std::fprintf(stderr, "cf_turn_standin: %s\n", err.c_str());
        return 1;
    }
    std::printf("cf_turn_standin: TURN / STUN on %s (user %s)\n", srv.addr().text().c_str(), user.c_str());
    std::fflush(stdout);
    unsigned long long last = ~0ull;
    int lastAllocs = -1;
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (srv.relayedBytes() != last || srv.allocations() != lastAllocs) {
            last = srv.relayedBytes();
            lastAllocs = srv.allocations();
            std::printf("cf_turn_standin: %d allocations, %llu bytes relayed\n", lastAllocs, last);
            std::fflush(stdout);
        }
    }
}
