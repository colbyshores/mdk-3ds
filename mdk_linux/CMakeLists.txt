cmake_minimum_required(VERSION 3.16)
project(mdk LANGUAGES C CXX)

# ============================================================================
# mdk_linux — the Linux frontend and the Vulkan 1.3 renderer.
#
# The two-tree layout: ../mdk_core/src is the shared game (compiled into this
# target AND into mdk_3ds), and src/ is this tree's platform layer (the SDL3
# window, input, audio sink) plus the Vulkan backend that implements the
# render.h seam.
#
# The 3DS tree is ../mdk_3ds and is never referenced here.
# ============================================================================

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CORE "${CMAKE_CURRENT_SOURCE_DIR}/../mdk_core")
get_filename_component(CORE "${CORE}" ABSOLUTE)

# Default to an OPTIMISED build. With no CMAKE_BUILD_TYPE set at all, CMake's
# own default is unoptimised (no -O flag), while mdk_3ds/Makefile always
# builds -O2. RelWithDebInfo (-O2 -g) keeps it debuggable. See the
# -fno-strict-aliasing note below for why the two builds must optimise alike.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# ----------------------------------------------------------------------------
# Dependencies
# ----------------------------------------------------------------------------

# --- SDL3 (window, input, gamepads, touch, audio stream) ---
find_package(SDL3 REQUIRED)

# --- Vulkan: vendored headers + volk. We do NOT link against libvulkan:
# volk dlopens the loader, so a machine without an ICD gets a message, not a
# missing .so.
set(VK_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vulkan/include")
set(VOLK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/volk")
if(NOT EXISTS "${VK_INCLUDE_DIR}/vulkan/vulkan.h" OR NOT EXISTS "${VOLK_DIR}/volk.h")
    message(FATAL_ERROR "vendored vulkan headers / volk missing under ${CMAKE_CURRENT_SOURCE_DIR}/third_party")
endif()

# --- glslang (GLSL -> SPIR-V). Searched in the SDK / distro locations and the
# user-local prebuilt (~/.local/opt/glslang/bin), which is where the sibling
# t2k project keeps it.
find_program(GLSLANG NAMES glslangValidator glslang
    HINTS $ENV{VULKAN_SDK}/bin $ENV{HOME}/.local/opt/glslang/bin /usr/local/bin /usr/bin)
if(NOT GLSLANG)
    message(FATAL_ERROR "glslang/glslangValidator not found (install glslang-tools, the Vulkan SDK, or the prebuilt into ~/.local/opt/glslang)")
endif()
message(STATUS "glslang: ${GLSLANG}")

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

# ----------------------------------------------------------------------------
# Sources
#
# THE CORE SOURCES ARE LISTED EXPLICITLY, NOT GLOBBED. The 3DS Makefile
# globs; this one lists. The reason: a core source that lands on one target
# only is not "phase one", it is a SILENTLY DIFFERENT GAME. An explicit list
# makes the omission a build error. Every source added to mdk_core/src must
# be added here AND picked up by mdk_3ds/Makefile's glob.
# ----------------------------------------------------------------------------
set(SOURCES
    # ---- platform layer (this tree) ----
    src/main.cpp
    src/audio/audio_sdl.cpp
    src/platform/window_sdl.cpp
    src/platform/filesystem_posix.cpp

    # ---- the Vulkan renderer behind the render.h seam (RENDERER_VK) ----
    src/render/renderer_vk.cpp
    src/render/system_numerics_inverse.cpp
    src/render/vk/vk_context.cpp
    third_party/volk/volk.c

    # ---- mdk_core: the shared game, compiled into BOTH targets ----
    ${CORE}/src/audio/limiter.cpp
    ${CORE}/src/audio/sound.cpp
    ${CORE}/src/audio/sound_bank.cpp
    ${CORE}/src/audio/sound_device.cpp
    ${CORE}/src/audio/sound_mixer.cpp
    ${CORE}/src/formats/arena.cpp
    ${CORE}/src/formats/beta_demo.cpp
    ${CORE}/src/formats/bin_reader.cpp
    ${CORE}/src/formats/bni.cpp
    ${CORE}/src/formats/case_insensitive_path.cpp
    ${CORE}/src/formats/cmi.cpp
    ${CORE}/src/formats/cmi_beta.cpp
    ${CORE}/src/formats/dti.cpp
    ${CORE}/src/formats/fall3d.cpp
    ${CORE}/src/formats/file_system.cpp
    ${CORE}/src/formats/flc.cpp
    ${CORE}/src/formats/font.cpp
    ${CORE}/src/formats/fti.cpp
    ${CORE}/src/formats/gif.cpp
    ${CORE}/src/formats/glb.cpp
    ${CORE}/src/formats/local_paths.cpp
    ${CORE}/src/formats/mdk_data.cpp
    ${CORE}/src/formats/model.cpp
    ${CORE}/src/formats/model_animation.cpp
    ${CORE}/src/formats/mto.cpp
    ${CORE}/src/formats/mve.cpp
    ${CORE}/src/formats/palette.cpp
    ${CORE}/src/formats/png.cpp
    ${CORE}/src/formats/script_decoder.cpp
    ${CORE}/src/formats/script_decoder_beta.cpp
    ${CORE}/src/formats/script_opcodes.cpp
    ${CORE}/src/formats/sni.cpp
    ${CORE}/src/formats/sniper_screen.cpp
    ${CORE}/src/formats/sprite_animation.cpp
    ${CORE}/src/formats/texture.cpp
    ${CORE}/src/formats/texture_archive.cpp
    ${CORE}/src/formats/wav.cpp
    ${CORE}/src/game/arena_space.cpp
    ${CORE}/src/game/bsp.cpp
    ${CORE}/src/game/camera_roll.cpp
    ${CORE}/src/game/follow_camera.cpp
    ${CORE}/src/game/frame_blend.cpp
    ${CORE}/src/game/game_flow.cpp
    ${CORE}/src/game/game_state.cpp
    ${CORE}/src/game/hud_view.cpp
    ${CORE}/src/game/kurt.cpp
    ${CORE}/src/game/kurt_sprite.cpp
    ${CORE}/src/game/level_data.cpp
    ${CORE}/src/game/arena_stream.cpp
    ${CORE}/src/game/arena_views.cpp
    ${CORE}/src/game/level_flow.cpp
    ${CORE}/src/game/enhanced_config.cpp
    ${CORE}/src/game/enhanced_menu.cpp
    ${CORE}/src/game/font_view.cpp
    ${CORE}/src/game/hd_key.cpp
    ${CORE}/src/game/mod_catalog.cpp
    ${CORE}/src/game/mod_images.cpp
    ${CORE}/src/game/mod_info.cpp
    ${CORE}/src/game/mod_models.cpp
    ${CORE}/src/game/part_fit.cpp
    ${CORE}/src/util/sha256.cpp
    ${CORE}/src/game/level_light.cpp
    ${CORE}/src/game/level_view.cpp
    ${CORE}/src/game/effects_draw.cpp
    ${CORE}/src/game/effects_view.cpp
    ${CORE}/src/game/object_draw.cpp
    ${CORE}/src/game/object_view.cpp
    ${CORE}/src/game/player.cpp
    ${CORE}/src/render/arena_scene.cpp
    ${CORE}/src/render/colour_mips.cpp
    ${CORE}/src/render/post_params.cpp
    ${CORE}/src/render/sun_shadow.cpp
    ${CORE}/src/render/camera_builder.cpp
    ${CORE}/src/render/depth_pull.cpp
    ${CORE}/src/render/sky_dome.cpp
    ${CORE}/src/render/draw_queue.cpp
    ${CORE}/src/render/material_resolver.cpp
    ${CORE}/src/render/surface_cache.cpp
    ${CORE}/src/scripts/object.cpp
    ${CORE}/src/scripts/animated_textures.cpp
    ${CORE}/src/scripts/effects.cpp
    ${CORE}/src/scripts/fans.cpp
    ${CORE}/src/scripts/pose_cache.cpp
    ${CORE}/src/scripts/script_runtime.cpp
    ${CORE}/src/scripts/script_vm.cpp
    ${CORE}/src/scripts/debris.cpp
    ${CORE}/src/util/math_lut.cpp
    ${CORE}/src/util/random.cpp
)

# ----------------------------------------------------------------------------
# Shaders: GLSL -> SPIR-V (glslang) -> a C header (spv2h.py)
# ----------------------------------------------------------------------------
set(SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(GEN_DIR "${CMAKE_BINARY_DIR}/generated")
set(SHADERS
    palette.vert palette.frag
    mirror.frag
    sky.vert sky.frag
    stereo.vert stereo.frag
    enhanced.vert enhanced.frag
    gamma.frag
    depth.frag
    enhanced.frag
    occlusion.frag
    post.frag
    stereo.frag
)
set(SHADER_HEADERS)
foreach(sh ${SHADERS})
    if(NOT EXISTS "${SHADER_DIR}/${sh}")
        continue()
    endif()
    string(REPLACE "." "_" sym "${sh}")
    set(spv "${GEN_DIR}/${sh}.spv")
    set(hdr "${GEN_DIR}/shader_${sym}.h")
    add_custom_command(
        OUTPUT "${spv}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GEN_DIR}"
        COMMAND ${GLSLANG} -V --target-env vulkan1.3 -I"${SHADER_DIR}" -o "${spv}" "${SHADER_DIR}/${sh}"
        DEPENDS "${SHADER_DIR}/${sh}"
        COMMENT "GLSL ${sh}")
    add_custom_command(
        OUTPUT "${hdr}"
        COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/spv2h.py" "${spv}" "${hdr}" "spv_${sym}"
        DEPENDS "${spv}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/spv2h.py"
        COMMENT "SPV2H ${sh}")
    list(APPEND SHADER_HEADERS "${hdr}")
endforeach()

# ----------------------------------------------------------------------------
# Target
# ----------------------------------------------------------------------------
add_executable(mdk ${SOURCES} ${SHADER_HEADERS})

target_include_directories(mdk PRIVATE
    "${CORE}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${GEN_DIR}"
    "${VK_INCLUDE_DIR}"
    "${VOLK_DIR}"
)

# Rendering backend selection. The 3DS/Citro3D backend is selected by
# mdk_3ds/Makefile with -DRENDERER_C3D.
target_compile_definitions(mdk PRIVATE RENDERER_VK VK_NO_PROTOTYPES)

target_link_libraries(mdk PRIVATE SDL3::SDL3 Threads::Threads ZLIB::ZLIB)

if(UNIX AND NOT APPLE)
    target_link_libraries(mdk PRIVATE m dl)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(mdk PRIVATE -Wall -Wextra -Wno-unused-parameter)
    # Matches mdk_3ds/Makefile's COMMONFLAGS. Non-optional now that this build
    # is optimised: the shared code reads binary data through memcpy into
    # typed locals, and -O2 without this flag lets the optimiser assume
    # aliasing rules the code does not honour.
    target_compile_options(mdk PRIVATE -fno-strict-aliasing)
    # Matches mdk_3ds/Makefile: the engine is written exception-free /
    # RTTI-free (no try/catch/throw/dynamic_cast/typeid in mdk_core), so
    # enforce the same contract on the desktop build rather than only
    # asserting it in the docs.
    target_compile_options(mdk PRIVATE -fno-exceptions -fno-rtti)
    set_source_files_properties(third_party/volk/volk.c PROPERTIES COMPILE_FLAGS "-w")
endif()
