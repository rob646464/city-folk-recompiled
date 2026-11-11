# The self-hosted lobby and its API test (docs/ONLINE.md "The lobby"). Included from the top-level
# CMakeLists.txt, so paths are relative to the repository root.
#
# cf_lobby is one self-contained C++17 file; server owners can also build it without CMake:
#   g++ -O2 -std=c++17 -pthread tools/lobby/cf_lobby.cpp -o cf_lobby
find_package(Threads REQUIRED)

add_executable(cf_lobby tools/lobby/cf_lobby.cpp)
if (WIN32)
    target_link_libraries(cf_lobby PRIVATE ws2_32)
else ()
    target_link_libraries(cf_lobby PRIVATE Threads::Threads)
endif ()

# The API test: cf_lobby as a library (lobbyMain in a thread, loopback port chosen by the OS) driven by
# the game's own client code (src/port/http.cpp: request() and WebSocket) and plain sockets.
add_executable(cf_lobby_api_test tools/tests/lobby_api_test.cpp tools/lobby/cf_lobby.cpp src/port/http.cpp)
target_include_directories(cf_lobby_api_test PRIVATE src)
target_compile_definitions(cf_lobby_api_test PRIVATE CF_LOBBY_AS_LIB)
if (WIN32)
    target_link_libraries(cf_lobby_api_test PRIVATE ws2_32 winhttp)
else ()
    # http.cpp loads libcurl at run time (dlopen).
    target_link_libraries(cf_lobby_api_test PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif ()
add_test(NAME lobby_api COMMAND cf_lobby_api_test)

# Lobby rooms end to end: hosts and joiners (src/recomp/online_net.* through online_lobby.*) meet on
# cf_lobby in a thread and open their UDP paths through simulated routers (tools/tests/nat_sim.h: full
# cone, port restricted, symmetric) with the TURN / STUN stand-in. Loopback only, about 100 s.
add_executable(cf_online_lobby_test tools/tests/online_lobby_test.cpp tools/tests/turn_standin.cpp tools/lobby/cf_lobby.cpp
    src/port/http.cpp src/recomp/online_lobby.cpp src/recomp/online_net.cpp src/recomp/online_turn.cpp src/recomp/online_nat.cpp
    src/recomp/online_crypto.cpp src/recomp/online_ed25519.cpp)
target_include_directories(cf_online_lobby_test PRIVATE src)
target_compile_definitions(cf_online_lobby_test PRIVATE CF_LOBBY_AS_LIB)
if (WIN32)
    target_link_libraries(cf_online_lobby_test PRIVATE ws2_32 iphlpapi bcrypt winhttp)
else ()
    target_link_libraries(cf_online_lobby_test PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
endif ()
add_test(NAME online_lobby COMMAND cf_online_lobby_test)
