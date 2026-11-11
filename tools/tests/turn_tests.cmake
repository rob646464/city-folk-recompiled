# TURN client (src/recomp/online_turn.*) against the in-process test server (tools/tests/turn_standin.*),
# loopback only. Included from the top-level CMakeLists.txt.
add_executable(cf_turn_test tools/tests/turn_test.cpp tools/tests/turn_standin.cpp src/recomp/online_turn.cpp
    src/recomp/online_net.cpp src/recomp/online_nat.cpp src/recomp/online_crypto.cpp src/recomp/online_ed25519.cpp)
target_include_directories(cf_turn_test PRIVATE src)
if (WIN32)
    target_link_libraries(cf_turn_test PRIVATE ws2_32 iphlpapi bcrypt)
endif ()
add_test(NAME online_turn COMMAND cf_turn_test)

# The stand-in as a program (two local game instances: relay and STUN without the internet).
add_executable(cf_turn_standin tools/tests/turn_standin_main.cpp tools/tests/turn_standin.cpp src/recomp/online_turn.cpp
    src/recomp/online_net.cpp src/recomp/online_nat.cpp src/recomp/online_crypto.cpp src/recomp/online_ed25519.cpp)
target_include_directories(cf_turn_standin PRIVATE src tools/tests)
if (WIN32)
    target_link_libraries(cf_turn_standin PRIVATE ws2_32 iphlpapi bcrypt)
endif ()
