cmake_minimum_required(VERSION 3.25)
project(cfport LANGUAGES C CXX VERSION 0.0.1)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(AURORA_ENABLE_DVD ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD ON CACHE BOOL "" FORCE)
# Local fixes to Aurora, kept as patches so the submodule stays pristine (the same series as the
# Luigi's Mansion port; City Folk-only patches are numbered 0300+, docs/BACKPORT.md).
# The applied set is recorded in a stamp (hash of every patch, in order); when the set changes,
# the submodule is reset to pristine and the whole series re-applied, since later patches may
# touch the same lines as earlier ones (a per-patch reverse check can't tell what's applied).
file(GLOB AURORA_PATCHES ${CMAKE_SOURCE_DIR}/patches/aurora/*.patch)
list(SORT AURORA_PATCHES)
set(_patch_ids "")
foreach (_patch ${AURORA_PATCHES})
    file(SHA1 ${_patch} _sha)
    get_filename_component(_name ${_patch} NAME)
    string(APPEND _patch_ids "${_name}:${_sha}\n")
endforeach ()
set(_aurora_dir ${CMAKE_SOURCE_DIR}/extern/aurora)
execute_process(COMMAND git rev-parse --git-dir WORKING_DIRECTORY ${_aurora_dir}
                OUTPUT_VARIABLE _aurora_git OUTPUT_STRIP_TRAILING_WHITESPACE)
get_filename_component(_aurora_git ${_aurora_git} ABSOLUTE BASE_DIR ${_aurora_dir})
set(_stamp ${_aurora_git}/cf_patches_applied)
set(_stamped "")
if (EXISTS ${_stamp})
    file(READ ${_stamp} _stamped)
endif ()
if (NOT _stamped STREQUAL _patch_ids)
    # The patches are LF; Git for Windows' system-wide core.autocrlf=true would check the
    # submodule out with CRLF. Force LF and rewrite every file before applying.
    execute_process(COMMAND git config core.autocrlf false WORKING_DIRECTORY ${_aurora_dir})
    execute_process(COMMAND git checkout -q -- . WORKING_DIRECTORY ${_aurora_dir})
    execute_process(COMMAND git clean -fdq WORKING_DIRECTORY ${_aurora_dir})
    execute_process(COMMAND git rm -r -q --cached . WORKING_DIRECTORY ${_aurora_dir} OUTPUT_QUIET ERROR_QUIET)
    execute_process(COMMAND git reset -q --hard WORKING_DIRECTORY ${_aurora_dir})
    # (files a patch added stay behind as untracked after the reset when they were staged)
    execute_process(COMMAND git clean -fdq WORKING_DIRECTORY ${_aurora_dir})
    execute_process(COMMAND git checkout-index -f -a WORKING_DIRECTORY ${_aurora_dir})
    foreach (_patch ${AURORA_PATCHES})
        execute_process(COMMAND git apply ${_patch} WORKING_DIRECTORY ${_aurora_dir} RESULT_VARIABLE _applied)
        if (NOT _applied EQUAL 0)
            file(REMOVE ${_stamp})
            message(FATAL_ERROR "Failed to apply Aurora patch ${_patch}")
        endif ()
        message(STATUS "Applied Aurora patch ${_patch}")
    endforeach ()
    file(WRITE ${_stamp} "${_patch_ids}")
endif ()
add_subdirectory(extern/aurora EXCLUDE_FROM_ALL)

if (WIN32)
    # dxcompiler/dxil are loaded dynamically by Dawn's D3D12 backend, so they are not part of
    # TARGET_RUNTIME_DLLS and must be copied explicitly.
    file(GLOB _dawn_extra_dlls "${CMAKE_BINARY_DIR}/_deps/dawn_prebuilt-src/bin/dx*.dll")
endif ()

# ---- Recompiler (docs/RECOMPILER.md, docs/SETUP.md) ---------------------------
# The recompiler (src/recompiler) translates the user's own main.dol and its REL modules into
# C++. It is linked into the shipped executable (first-run setup) and into the cfrecompile tool
# the dev build uses. Nothing generated is ever committed.
add_library(cf_recompiler STATIC src/recompiler/recompiler.cpp src/recompiler/recompiler.h)
target_include_directories(cf_recompiler PUBLIC src)
add_executable(cfrecompile tools/recomp/cfrecompile.cpp)
target_link_libraries(cfrecompile PRIVATE cf_recompiler)

# Real Wii Remote protocol (src/recomp/wiimote_proto.cpp) against a fake HID device: no hardware,
# no SDL (docs/CONTROLS.md "Real Wii Remotes"). ctest or build/<preset>/cf_wiimote_hid_test.
add_executable(cf_wiimote_hid_test tools/tests/wiimote_hid_test.cpp src/recomp/wiimote_proto.cpp)
target_include_directories(cf_wiimote_hid_test PRIVATE src)
enable_testing()
add_test(NAME wiimote_hid COMMAND cf_wiimote_hid_test)
# Nintendo events: WC24 crypto (SHA-1, AES-OFB, RSA) and VFF volumes (docs/DISTRIBUTIONS.md "Testing").
add_executable(cf_distrib_test tools/tests/distrib_test.cpp src/recomp/wc24_crypto.cpp src/recomp/vff.cpp)
target_include_directories(cf_distrib_test PRIVATE src)
add_test(NAME distrib COMMAND cf_distrib_test)
# Mii data / database checks (docs/MIIS.md).
add_executable(cf_mii_test tools/tests/mii_test.cpp src/game/cf_mii_data.cpp)
target_include_directories(cf_mii_test PRIVATE src)
add_test(NAME mii COMMAND cf_mii_test)

# Online room transport (src/recomp/online_net.*, docs/ONLINE.md): host + members over loopback UDP.
add_executable(cf_online_test tools/tests/online_net_test.cpp src/recomp/online_net.cpp
    src/recomp/online_nat.cpp src/recomp/online_crypto.cpp src/recomp/online_ed25519.cpp)
target_include_directories(cf_online_test PRIVATE src)
if (WIN32)
    target_link_libraries(cf_online_test PRIVATE ws2_32 iphlpapi bcrypt)
endif ()
add_test(NAME online_net COMMAND cf_online_test)

# Runtime shared by both executables: memory/CPU helpers, scheduler, HLE SDK, frame driver,
# audio, menu, and the game-specific parts in src/game. module_rt.cpp belongs to the game
# module, not the runtime.
file(GLOB RECOMP_RUNTIME_SOURCES CONFIGURE_DEPENDS src/recomp/*.cpp src/game/*.cpp)
list(REMOVE_ITEM RECOMP_RUNTIME_SOURCES ${CMAKE_SOURCE_DIR}/src/recomp/module_rt.cpp)
list(APPEND RECOMP_RUNTIME_SOURCES src/port/crash_handler.cpp src/port/options.cpp src/port/dol.cpp
    src/port/settings.cpp src/port/install.cpp src/port/update.cpp src/port/recovery.cpp src/port/disc_folder.cpp
    src/port/http.cpp)
set_source_files_properties(src/port/update.cpp PROPERTIES COMPILE_DEFINITIONS "CF_VERSION=\"${PROJECT_VERSION}\"")

function(cf_runtime_target tgt)
    target_include_directories(${tgt} PRIVATE src
        ${CMAKE_SOURCE_DIR}/extern/aurora/include/dolphin ${CMAKE_SOURCE_DIR}/extern/aurora/include)
    target_compile_definitions(${tgt} PRIVATE TARGET_PC=1)
    target_link_libraries(${tgt} PRIVATE aurora::main aurora::core aurora::gx aurora::vi aurora::pad
        aurora::si aurora::os aurora::dvd aurora::card)
    # Shipped shader/pipeline precache list (docs/RENDERING.md "Pipeline precache").
    if (EXISTS ${CMAKE_SOURCE_DIR}/data/pipelines.bin)
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/data/pipelines.bin $<TARGET_FILE_DIR:${tgt}>)
    endif ()
    if (WIN32)
        target_link_libraries(${tgt} PRIVATE dbghelp winhttp bcrypt ws2_32 iphlpapi)  # winhttp/bcrypt: updater; ws2_32/iphlpapi: online
        target_sources(${tgt} PRIVATE ${CMAKE_SOURCE_DIR}/packaging/windows/longpath.manifest)
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:${tgt}> $<TARGET_FILE_DIR:${tgt}>
            COMMAND_EXPAND_LISTS)
        if (_dawn_extra_dlls)
            add_custom_command(TARGET ${tgt} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_dawn_extra_dlls} $<TARGET_FILE_DIR:${tgt}>)
        endif ()
    elseif (APPLE)
        target_compile_options(${tgt} PRIVATE -ffp-contract=off)
    elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_sources(${tgt} PRIVATE ${CMAKE_SOURCE_DIR}/src/port/glibc_compat.c)
        target_link_options(${tgt} PRIVATE -static-libstdc++)
        target_link_libraries(${tgt} PRIVATE ${CMAKE_DL_LIBS})
    endif ()
endfunction()

# Function maps: the DOL's (config/<id>/symbols.txt) and one per REL module
# (config/<id>/rels/<module>.txt). Missing ones are filled by the recompiler's own scan.
set(CF_GAME_ID "RUUE01" CACHE STRING "Game id of CF_DISC (RUUE01 USA rev 1; RUUP01, RUUJ01, RUUK01)")
set(CF_CONFIG_DIR ${CMAKE_SOURCE_DIR}/config/${CF_GAME_ID})

# CityFolkRecomp: what end users run. It contains the runtime, the recompiler and the setup UI;
# the game itself is a module DLL that setup compiles from the user's disc with the bundled
# clang + lld (docs/SETUP.md).
file(GLOB SETUP_SOURCES CONFIGURE_DEPENDS src/setup/*.cpp src/setup/*.h)
add_executable(CityFolkRecomp ${RECOMP_RUNTIME_SOURCES} ${SETUP_SOURCES})
cf_runtime_target(CityFolkRecomp)
target_link_libraries(CityFolkRecomp PRIVATE cf_recompiler)  # nod comes with aurora::dvd
if (WIN32)
    set_target_properties(CityFolkRecomp PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_options(CityFolkRecomp PRIVATE "LINKER:/ENTRY:mainCRTStartup")
endif ()
add_executable(cfembed tools/recomp/cfembed.cpp)
# Setup's inputs: the function tables of every supported release (setup recompiles whichever disc
# the player has; docs/SETUP.md), the hook list and the module's freestanding sources.
set(CF_EMBEDDED_FILES
    kHooks=tools/recomp/hooks.txt kCtxHeader=src/recomp/ctx.h kModuleAbiHeader=src/recomp/module_abi.h
    kModuleRt=src/recomp/module_rt.cpp kRecompilerSource=src/recompiler/recompiler.cpp)
set(_embed_deps)
foreach (_id RUUE01 RUUP01 RUUJ01 RUUK01)
    list(APPEND CF_EMBEDDED_FILES kSymbols_${_id}=config/${_id}/symbols.txt kSplits_${_id}=config/${_id}/splits.txt
        kModules_${_id}=config/${_id}/modules.txt)
    if (NOT _id STREQUAL "RUUE01")
        list(APPEND CF_EMBEDDED_FILES kUsMap_${_id}=config/${_id}/us_map.txt kRelUsMap_${_id}=config/${_id}/rels_us_map.txt)
    endif ()
    # The REL maps: one embedded blob of the whole folder (cfembed's folder mode).
    file(GLOB _embed_rel_maps CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/config/${_id}/rels/*.txt)
    list(APPEND _embed_deps ${_embed_rel_maps})
endforeach ()
foreach (_e ${CF_EMBEDDED_FILES})
    string(REGEX REPLACE "^[^=]*=" "${CMAKE_SOURCE_DIR}/" _f ${_e})
    list(APPEND _embed_deps ${_f})
endforeach ()
foreach (_id RUUE01 RUUP01 RUUJ01 RUUK01)
    list(APPEND CF_EMBEDDED_FILES kRelMaps_${_id}=config/${_id}/rels)
endforeach ()
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/embedded_data.cpp
    COMMAND cfembed ${CMAKE_BINARY_DIR}/embedded_data.cpp setup::embedded::data ${CF_EMBEDDED_FILES}
    DEPENDS cfembed ${_embed_deps}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Embedding setup data"
    VERBATIM)
target_sources(CityFolkRecomp PRIVATE ${CMAKE_BINARY_DIR}/embedded_data.cpp)
if (WIN32)
    set(CF_MODULE_LINKER lld-link)
elseif (APPLE)
    set(CF_MODULE_LINKER ld64.lld)
else ()
    set(CF_MODULE_LINKER ld.lld)
endif ()
get_filename_component(_cxx_dir ${CMAKE_CXX_COMPILER} DIRECTORY)
if (EXISTS ${_cxx_dir}/clang${CMAKE_EXECUTABLE_SUFFIX} AND EXISTS ${_cxx_dir}/${CF_MODULE_LINKER}${CMAKE_EXECUTABLE_SUFFIX})
    target_compile_definitions(CityFolkRecomp PRIVATE "CF_DEV_LLVM_DIR=\"${_cxx_dir}\"")
endif ()

# Packages (docs/SETUP.md "Packages"; CI: .github/workflows/build.yml): CF_LLVM_DIR (an LLVM bin
# directory) supplies the bundled clang and lld-link / ld.lld / ld64.lld.
set(CF_LLVM_DIR "${_cxx_dir}" CACHE PATH "LLVM bin directory whose clang and lld-link (Windows) / ld.lld (Linux) / ld64.lld (macOS) are bundled on install")
install(TARGETS CityFolkRecomp RUNTIME DESTINATION .)
install(FILES data/pipelines.bin DESTINATION . OPTIONAL)
install(FILES packaging/LLVM-LICENSE.TXT DESTINATION llvm)
if (WIN32)
    install(FILES packaging/windows/README.txt DESTINATION .)
    install(CODE "
        file(GLOB _dlls \"$<TARGET_FILE_DIR:CityFolkRecomp>/*.dll\")
        file(INSTALL \${_dlls} DESTINATION \"\${CMAKE_INSTALL_PREFIX}\")")
    install(PROGRAMS ${CF_LLVM_DIR}/clang${CMAKE_EXECUTABLE_SUFFIX} ${CF_LLVM_DIR}/lld-link${CMAKE_EXECUTABLE_SUFFIX}
        DESTINATION llvm/bin OPTIONAL)
else ()
    # Linux / macOS: clang and the linker are usually symlinks (clang -> clang-23, ld.lld -> lld);
    # copy the real binaries under the names setup looks for (lld picks its flavour from its
    # name). Use a self-contained LLVM (the official release archive), not a distribution's clang
    # that needs libLLVM/libclang-cpp next to it.
    install(CODE "
        foreach (_name clang ${CF_MODULE_LINKER})
            set(_from \"${CF_LLVM_DIR}/\${_name}\")
            if (NOT EXISTS \"\${_from}\")
                message(WARNING \"CF_LLVM_DIR has no \${_name}; the package will lack the bundled compiler\")
                continue()
            endif ()
            file(REAL_PATH \"\${_from}\" _real)
            file(MAKE_DIRECTORY \"\${CMAKE_INSTALL_PREFIX}/llvm/bin\")
            file(COPY_FILE \"\${_real}\" \"\${CMAKE_INSTALL_PREFIX}/llvm/bin/\${_name}\")
            file(CHMOD \"\${CMAKE_INSTALL_PREFIX}/llvm/bin/\${_name}\" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                 GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
            message(STATUS \"Bundled \${_real} as llvm/bin/\${_name}\")
        endforeach ()")
endif ()
if (APPLE)
    install(FILES packaging/macos/README.txt DESTINATION .)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # The official LLVM lld needs libxml2.so.2 (only for lld-link manifests): ship a stub in
    # llvm/lib, found through lld's RUNPATH $ORIGIN/../lib (packaging/linux/libxml2_stub.c).
    add_library(cf_libxml2_stub SHARED packaging/linux/libxml2_stub.c)
    set_target_properties(cf_libxml2_stub PROPERTIES OUTPUT_NAME xml2 SOVERSION 2
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/libxml2_stub)
    target_link_options(cf_libxml2_stub PRIVATE
        -Wl,--version-script=${CMAKE_SOURCE_DIR}/packaging/linux/libxml2_stub.map)
    add_dependencies(CityFolkRecomp cf_libxml2_stub)
    install(FILES $<TARGET_FILE:cf_libxml2_stub> DESTINATION llvm/lib)
    install(PROGRAMS packaging/linux/CityFolkRecompiled.sh DESTINATION .)
    install(FILES packaging/linux/README.txt DESTINATION .)
endif ()

# cfrecomp: developer build with the game recompiled at build time and linked in statically
# (debuggable, no setup). CF_DISC is the user's own disc: an extracted Wii game partition (a
# folder with sys/main.dol, sys/fst.bin and files/, as written by Dolphin's "Extract Entire
# Disc") or a disc image.
set(CF_DISC "${CMAKE_SOURCE_DIR}/US DATA/DATA" CACHE PATH
    "Your Animal Crossing: City Folk disc: extracted game partition (sys/, files/) or image file")
set(CF_DOL "${CF_DISC}/sys/main.dol" CACHE FILEPATH "main.dol from your own disc (extracted)")
set(CF_RELS "${CF_DISC}/files/rels" CACHE PATH "REL modules from your own disc (extracted files/rels)")
if (EXISTS ${CF_DOL})
    set(CF_RECOMP_DIR ${CMAKE_BINARY_DIR}/recomp_gen)
    file(GLOB _rel_files CONFIGURE_DEPENDS "${CF_RELS}/*.rel")
    file(GLOB _rel_maps CONFIGURE_DEPENDS "${CF_CONFIG_DIR}/rels/*.txt")
    set(_recomp_cfg --symbols ${CF_CONFIG_DIR}/symbols.txt)
    set(_recomp_deps ${CF_CONFIG_DIR}/symbols.txt ${_rel_files} ${_rel_maps})
    if (EXISTS ${CF_CONFIG_DIR}/splits.txt)
        list(APPEND _recomp_cfg --splits ${CF_CONFIG_DIR}/splits.txt)
        list(APPEND _recomp_deps ${CF_CONFIG_DIR}/splits.txt)
    endif ()
    # Non-USA versions: the USA maps for hooks.txt "all" entries (docs/SETUP.md "Version-aware code").
    if (EXISTS ${CF_CONFIG_DIR}/us_map.txt)
        list(APPEND _recomp_cfg --us-map ${CF_CONFIG_DIR}/us_map.txt --rel-us-map ${CF_CONFIG_DIR}/rels_us_map.txt)
        list(APPEND _recomp_deps ${CF_CONFIG_DIR}/us_map.txt ${CF_CONFIG_DIR}/rels_us_map.txt)
    endif ()
    if (_rel_files)
        list(APPEND _recomp_cfg --rels ${CF_RELS} --rel-maps ${CF_CONFIG_DIR}/rels)
    endif ()
    # Fixed set of outputs (cfrc::kChunks in src/recompiler/recompiler.h).
    set(_recomp_files ${CF_RECOMP_DIR}/func_table.cpp)
    foreach (_i RANGE 63)
        string(LENGTH "${_i}" _len)
        if (_len EQUAL 1)
            set(_i "00${_i}")
        else ()
            set(_i "0${_i}")
        endif ()
        list(APPEND _recomp_files ${CF_RECOMP_DIR}/funcs_${_i}.cpp)
    endforeach ()
    add_custom_command(
        OUTPUT ${_recomp_files} ${CF_RECOMP_DIR}/funcs.h ${CF_RECOMP_DIR}/summary.txt
        COMMAND cfrecompile --dol ${CF_DOL} --version ${CF_GAME_ID} ${_recomp_cfg}
            --hooks ${CMAKE_SOURCE_DIR}/tools/recomp/hooks.txt --out ${CF_RECOMP_DIR}
        DEPENDS cfrecompile ${CF_DOL} ${_recomp_deps} ${CMAKE_SOURCE_DIR}/tools/recomp/hooks.txt
        COMMENT "Recompiling ${CF_DOL} and its REL modules"
        VERBATIM)

    add_executable(cfrecomp ${RECOMP_RUNTIME_SOURCES} ${_recomp_files})
    cf_runtime_target(cfrecomp)
    target_include_directories(cfrecomp PRIVATE ${CF_RECOMP_DIR})
    target_compile_definitions(cfrecomp PRIVATE CF_STATIC_MODULE=1 "CF_DEV_DISC=\"${CF_DISC}\"")
    # Generated code: one huge function per guest function, lots of gotos.
    if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        set_source_files_properties(${_recomp_files} PROPERTIES COMPILE_OPTIONS "/clang:-Wno-unused-label;/clang:-Wno-unused-variable")
    else ()
        set_source_files_properties(${_recomp_files} PROPERTIES COMPILE_OPTIONS "-Wno-unused-label;-Wno-unused-variable")
    endif ()
else ()
    message(STATUS "CF_DOL not found (${CF_DOL}); skipping the cfrecomp developer build")
endif ()
