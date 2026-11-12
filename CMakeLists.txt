cmake_minimum_required(VERSION 3.20)
project(SuperMarioWorld2YoshisIslandSNESRecomp C CXX)

set(CMAKE_C_STANDARD 11)

# Release pin for lobby matching. Release builds stamp the VERSION file so two
# peers can prove they are the same build; everything else stamps "dev".
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" _GAME_VER_RAW)
string(STRIP "${_GAME_VER_RAW}" GAME_RELEASE_VERSION)
set(SNESRECOMP_BUILD_VERSION "" CACHE STRING
    "Lobby release pin (empty = Release uses VERSION, else dev)")
if(SNESRECOMP_BUILD_VERSION STREQUAL "")
    get_property(_is_multi GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(_is_multi)
        set(SNESRECOMP_BUILD_VERSION "$<IF:$<CONFIG:Release>,${GAME_RELEASE_VERSION},dev>")
    elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
        set(SNESRECOMP_BUILD_VERSION "${GAME_RELEASE_VERSION}")
    else()
        set(SNESRECOMP_BUILD_VERSION "dev")
    endif()
endif()

set(SNESRECOMP_ROOT "${CMAKE_SOURCE_DIR}/snesrecomp" CACHE PATH
    "Path to the snesrecomp checkout used by this game")
if(NOT EXISTS "${SNESRECOMP_ROOT}/runner/runner.cmake")
    message(FATAL_ERROR
        "snesrecomp submodule missing at ${SNESRECOMP_ROOT}.\n"
        "Run: git submodule update --init --recursive")
endif()
# Mod packages are part of every project, not an option. The launcher's Mods
# page, the .snesmod loader, the trusted-plugin registry and netplay's mod-set
# gate all hang off this one switch; a build without it cannot exchange mods
# with a peer that has them, and the framework says so at every lobby init.
# Must precede the include: runner.cmake reads it to add the loader sources.
set(SNESRECOMP_ENABLE_MODS ON CACHE BOOL
    "Mod package loader + trusted plugins (required for every title)" FORCE)
include(${SNESRECOMP_ROOT}/runner/runner.cmake)

add_executable(SuperMarioWorld2YoshisIslandSNESRecomp
    ${SNESRECOMP_RUNNER_SOURCES}
    src/main.c
    src/smw2_rtl.c
    src/smw2_sprites.c
    src/smw2_update.c
    src/host_contract.c
    src/game_rtl.c
    src/gen_stubs.c
    src/smw2_telemetry.c
)

# ROM identity. rom_identity.txt is the one place the digests live: the build
# turns it into snesrecomp_rom_identity.h (SNESRECOMP_ROM_EXPECTED_SHA256 and
# friends) for src/main.c, and tools/regen.sh, scripts/package_release.sh and
# the release workflow read the same file. A revision bump is one edit there.
snesrecomp_rom_identity(SuperMarioWorld2YoshisIslandSNESRecomp ${CMAKE_SOURCE_DIR}/rom_identity.txt)

# Generated recompiler output. Not committed: it is derived from a copyrighted
# ROM, so every developer regenerates it locally from their own copy -- and a
# release does not ship it either. With -DSNESRECOMP_SETUP_HOST=ON and an empty
# src/gen/, this builds the SETUP HOST instead: the same executable minus the
# recompiled code, whose only path is the launcher's Generate & rebuild wizard.
# That is what release zips contain; the player's machine produces the
# playable binary from their own ROM. Without the option an empty src/gen/ is
# still a hard error (the framework owns the message).
snesrecomp_target_generated_code(SuperMarioWorld2YoshisIslandSNESRecomp ${CMAKE_SOURCE_DIR}/src/gen)

# The mod catalog. This names WHERE the packages are; the framework owns the
# layout they are staged into beside the executable, and stages them on every
# build OF THE EXECUTABLE -- including the launcher's own Generate & rebuild on
# a player's machine, which builds only this target and would skip a sibling
# ALL target. mods/preloaded/packages/ holds reviewed, default-disabled
# packages; players install .snesmod archives through the launcher.
# snesrecomp/docs/MOD_PACKAGES.md.
#
# It used to be `snesrecomp_target_stage_dir(... ${CMAKE_SOURCE_DIR}/mods mods)`
# -- the destination spelled per title, which is how three ports in the same
# workspace ended up shipping an empty Mods page and nothing said so. Declaring
# it is now also enforced: a project with packages that never calls this fails
# to configure.
snesrecomp_target_mod_catalog(SuperMarioWorld2YoshisIslandSNESRecomp ${CMAKE_SOURCE_DIR}/mods/preloaded)
# Any other directory the game reads exe-relative (a translations/ table, ...)
# is staged with snesrecomp_target_stage_dir, never with a separate ALL target.

target_include_directories(SuperMarioWorld2YoshisIslandSNESRecomp PRIVATE
    ${SNESRECOMP_RUNNER_INCLUDE_DIRS}
    # common_rtl.c includes "config.h": the desktop key-map / Config contract
    # the framework host supplies (snesrecomp_target_desktop_host below).
    ${SNESRECOMP_ROOT}/runner/src/desktop
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/recomp
)
target_link_libraries(SuperMarioWorld2YoshisIslandSNESRecomp PRIVATE ${SNESRECOMP_RUNNER_LIBRARIES})
target_compile_definitions(SuperMarioWorld2YoshisIslandSNESRecomp PRIVATE
    SNES_GAME_VERSION="${SNESRECOMP_BUILD_VERSION}"
    SNES_GAME_TITLE="Super Mario World 2 Yoshis Island"
)
snesrecomp_target_sdl(SuperMarioWorld2YoshisIslandSNESRecomp)

# The desktop host: launcher flow, ROM resolution, config.ini + [KeyMap],
# window + SDL/OpenGL presenters, audio, gamepads, the save-state browser and
# rewind filmstrip, the pacing clock, crash handlers and the post-mortem
# report. src/main.c is the shim that names this title and supplies its
# hooks; the host is the framework's and improves on a submodule pull.
# Add TIER2 once this port reads the interpreter-gap manifest after each run.
snesrecomp_target_desktop_host(SuperMarioWorld2YoshisIslandSNESRecomp)
# Windows: a window app, so no console opens beside the game; diagnostics go
# to SuperMarioWorld2YoshisIslandSNESRecomp.log (src/main.c). -DSMW2_CONSOLE=ON brings
# the console back for debugging.
option(SMW2_CONSOLE "Build the Windows executable with a console window" OFF)
if(WIN32)
    if(SMW2_CONSOLE)
        target_compile_definitions(SuperMarioWorld2YoshisIslandSNESRecomp PRIVATE SMW2_CONSOLE=1)
    else()
        set_target_properties(SuperMarioWorld2YoshisIslandSNESRecomp PROPERTIES WIN32_EXECUTABLE TRUE)
        if(MSVC)
            # Keep the portable main() entry point instead of WinMain.
            target_link_options(SuperMarioWorld2YoshisIslandSNESRecomp PRIVATE /ENTRY:mainCRTStartup)
        endif()
    endif()
endif()
# Shader presets for the launcher's Shader list: the framework's built-ins plus
# the curated libretro scanline/CRT/upscaler set in assets/shaders (refreshed
# by tools/import_libretro_shaders.py).
snesrecomp_target_shader_presets(SuperMarioWorld2YoshisIslandSNESRecomp)
file(GLOB_RECURSE SMW2_SHADER_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/assets/shaders/*")
set_property(TARGET SuperMarioWorld2YoshisIslandSNESRecomp APPEND PROPERTY LINK_DEPENDS ${SMW2_SHADER_FILES})
add_custom_command(TARGET SuperMarioWorld2YoshisIslandSNESRecomp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets/shaders"
        "$<TARGET_FILE_DIR:SuperMarioWorld2YoshisIslandSNESRecomp>/assets/shaders"
    COMMENT "SuperMarioWorld2YoshisIslandSNESRecomp: staging libretro shader presets" VERBATIM)
# rom.cfg (the ROM the wizard set this project up from; git-ignored) is
# staged beside the executable when none is there yet, so the first launch
# opens on that dump rather than asking for one.
snesrecomp_target_rom_cache(SuperMarioWorld2YoshisIslandSNESRecomp)

# Shared Dear ImGui pre-boot launcher (ROM picker, verification, display /
# audio / input settings). Self-contained: it brings its own ImGui, GL link and
# staged assets, and defines RECOMP_LAUNCHER so src/main.c compiles its GUI path.
set(RECOMP_UI_ROOT "${CMAKE_SOURCE_DIR}/recomp-ui" CACHE PATH
    "Root directory of the recomp-ui launcher repo" FORCE)
if(NOT EXISTS "${RECOMP_UI_ROOT}/recomp_ui.cmake")
    message(FATAL_ERROR
        "recomp-ui missing at ${RECOMP_UI_ROOT}.\n"
        "Run: git submodule update --init --recursive recomp-ui")
endif()
include(${RECOMP_UI_ROOT}/recomp_ui.cmake)
# BOXART stages launcher_assets/img/boxart.tga beside the exe; the launcher
# shows assets/img/boxart.tga by default. EXISTS-guarded in recomp_ui.cmake,
# so this configures cleanly before any art is fetched.
recomp_target_launcher_ui(SuperMarioWorld2YoshisIslandSNESRecomp CONSOLE snes
    BOXART "${CMAKE_SOURCE_DIR}/launcher_assets/img/boxart.tga")

# Generate & rebuild from inside the launcher: pick ROM -> recompile locally
# -> cmake --build -> relaunch. The implementation is the framework's; this
# project contributes no configuration (snesrecomp_codegen_host_autowire in
# src/main.c). recomp-ui/src is on the include path because the host speaks
# RecompLauncherCGameInfo.
target_sources(SuperMarioWorld2YoshisIslandSNESRecomp PRIVATE
    ${SNESRECOMP_ROOT}/host/snesrecomp_codegen_host.c)
target_include_directories(SuperMarioWorld2YoshisIslandSNESRecomp PRIVATE
    ${SNESRECOMP_ROOT}/host
    ${RECOMP_UI_ROOT}/src)

# Netplay is not built into this target.
# No multitap: two seats, one controller per port.

# Synthetic widescreen regressions exercise the real shadow store without a ROM.
include(CTest)
if(BUILD_TESTING)
  add_executable(smw2_ws_test
      tests/smw2_ws_test.c
      ${SNESRECOMP_ROOT}/runner/src/snes/ws_shadow.c)
  target_include_directories(smw2_ws_test PRIVATE ${SNESRECOMP_ROOT}/runner/src)
  add_test(NAME smw2_ws_addresses COMMAND smw2_ws_test)
  # Artifact-path policy regressions exercise the real runner path helper.
  add_executable(artifact_path_test
      tests/artifact_path_test.c
      ${SNESRECOMP_ROOT}/runner/src/host_paths.c)
  target_include_directories(artifact_path_test PRIVATE ${SNESRECOMP_ROOT}/runner/src)
  target_compile_definitions(artifact_path_test PRIVATE
      SNESRECOMP_ARTIFACT_PATH_TESTING=1)
  add_test(NAME artifact_path COMMAND artifact_path_test)
  # Video recorder API behavior in both compile-gate states. The enabled
  # test uses an unavailable ffmpeg path and never starts a real encoder.
  add_executable(video_rec_off_test tests/video_rec_test.c)
  target_include_directories(video_rec_off_test PRIVATE
      ${SNESRECOMP_ROOT}/runner/src
      ${SNESRECOMP_ROOT}/runner/src/desktop)
  add_test(NAME video_rec_off COMMAND video_rec_off_test)

  add_executable(video_rec_on_test
      tests/video_rec_test.c
      ${SNESRECOMP_ROOT}/runner/src/desktop/video_rec.c
      ${SNESRECOMP_ROOT}/runner/src/host_paths.c)
  target_include_directories(video_rec_on_test PRIVATE
      ${SNESRECOMP_ROOT}/runner/src
      ${SNESRECOMP_ROOT}/runner/src/desktop)
  target_compile_definitions(video_rec_on_test PRIVATE
      SNESRECOMP_VIDEO_CAPTURE=1)
  add_test(NAME video_rec_on COMMAND video_rec_on_test)
  set_tests_properties(video_rec_on PROPERTIES
      ENVIRONMENT "SNESRECOMP_FFMPEG=/nonexistent")

  set_tests_properties(smw2_ws_addresses PROPERTIES
      ENVIRONMENT "SNESRECOMP_WS_BG_MARGINS=1;SNESRECOMP_WS_BG_MARGINS_DEBUG=0")

  # Task-4 widescreen completion: pure geometry, real PPU BG raster paths
  # (both renderers), production snapshot/rollback lifecycle gates (spliced
  # translation unit) and real src/main.c hook wiring. The runner builds,
  # runs and re-runs everything under ASan+UBSan.
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME smw2_task4_widescreen
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/run_smw2_task4.py)
    set_tests_properties(smw2_task4_widescreen PROPERTIES TIMEOUT 300)
  endif()
endif()
