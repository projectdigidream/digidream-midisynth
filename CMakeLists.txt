# midisynth -- C++/WASM MIDI synth engine (adjacent project; not yet wired
# into the engine's root build).
#
# Targets:
#   synth_core        -- platform-free core (the deterministic artifact)
#   midisynth_render  -- offline SMF -> WAV renderer (testing / golden files)
#   midisynth_tests   -- unit test runner
#
# WASM: configure with the Emscripten toolchain; synth_core compiles as-is
# and bindings/embind_api.cpp becomes the module entry.
cmake_minimum_required(VERSION 3.20)
project(midisynth CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

add_library(synth_core STATIC
  src/synth/tables.cpp
  src/synth/wavetables.cpp
  src/synth/wavetable_gen_additive.cpp
  src/synth/wavetable_gen_extra.cpp
  src/synth/wavetable_cache.cpp
  src/synth/json_min.cpp
  src/synth/patch_loader.cpp
  src/synth/song_config.cpp
  src/bindings/c_api.cpp
  src/synth/voice.cpp
  src/synth/voice_pool.cpp
  src/synth/synth_engine.cpp
  src/synth/effects/reverb.cpp
  src/midi/smf_parser.cpp
  src/track/track.cpp
  src/midi/tempo_map.cpp
  src/sequencer/song.cpp
  src/sequencer/sequencer.cpp
  src/sequencer/music_director.cpp
)

# The engine's fixed-point primitives (dnp/math/fixed.hpp) are reused; the
# audio path itself carries midisynth's own formats.
target_include_directories(synth_core PUBLIC
  src
  ${CMAKE_CURRENT_SOURCE_DIR}/../engine/core/include
)

target_compile_options(synth_core PUBLIC
  -Wall -Wextra -Werror
  # Determinism guard rails: no fast-math reassociation, no FP contraction --
  # the audio path is integer-only, but if a float ever leaks in it must not
  # also acquire per-target FMA differences silently.
  -fno-fast-math
  -ffp-contract=off
  -fno-strict-aliasing
)

if(NOT EMSCRIPTEN)
  add_executable(midisynth_render src/output/render_cli.cpp)
  target_link_libraries(midisynth_render synth_core)

  add_executable(midisynth_bake src/output/bake_cli.cpp)
  target_link_libraries(midisynth_bake synth_core)

  # Native realtime playback (miniaudio). The miniaudio TU is third-party
  # code and is exempt from -Werror.
  add_executable(midisynth_play
    src/output/play_cli.cpp
    src/output/miniaudio_impl.c)
  target_include_directories(midisynth_play PRIVATE third_party)
  target_link_libraries(midisynth_play synth_core)
  set_source_files_properties(src/output/miniaudio_impl.c PROPERTIES COMPILE_OPTIONS "-w")
  if(APPLE)
    target_link_libraries(midisynth_play "-framework CoreAudio" "-framework AudioToolbox"
                          "-framework AudioUnit" "-framework CoreFoundation")
  endif()
  enable_language(C)

  file(GLOB TEST_SOURCES test/unit/*.cpp)
  add_executable(midisynth_tests ${TEST_SOURCES})
  target_link_libraries(midisynth_tests synth_core)

  enable_testing()
  add_test(NAME unit COMMAND midisynth_tests)
endif()

if(EMSCRIPTEN)
  add_executable(midisynth_wasm src/bindings/embind_api.cpp)
  target_link_libraries(midisynth_wasm synth_core embind)
  set_target_properties(midisynth_wasm PROPERTIES
    LINK_FLAGS "-sMODULARIZE=1 -sEXPORT_NAME=createMidiSynth -sALLOW_MEMORY_GROWTH=1 -sMALLOC=emmalloc -sSTACK_SIZE=1048576"
  )
endif()
