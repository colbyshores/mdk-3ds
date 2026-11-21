# Makefile — the scripts' end-to-end checks (the level-3 tick-1 table).
#
# A HOST build, like tests/formats: it exercises the shared code against the
# real game data. The source list is explicit for the same reason.

CORE := ../../mdk_core/src
LINUX := ../../mdk_linux/src
# The 3DS platform layer, for the TWO pieces of it that are pure arithmetic and
# can be pinned here: render/pica_matrix.{h,cpp} and platform/input_3ds.{h,cpp}.
# The rest of that tree includes <3ds.h> and cannot be compiled on a host, which
# is why the matrix math was split out of renderer_c3d.cpp and the control
# mapping out of input_3ds_hid.cpp in the first place. Last on the include path
# so it can never shadow a core or Linux header.
3DS := ../../mdk_3ds/src
BUILD := build
CXX ?= g++
CXXFLAGS := -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter \
            -fno-strict-aliasing -I$(CORE) -I$(LINUX) -I. -I../formats -I$(3DS)
LDFLAGS :=

COMMON_SOURCES := \
	$(CORE)/audio/limiter.cpp \
	$(CORE)/audio/sound.cpp \
	$(CORE)/audio/sound_bank.cpp \
	$(CORE)/audio/sound_device.cpp \
	$(CORE)/audio/sound_mixer.cpp \
	$(CORE)/audio/level_music.cpp \
	$(CORE)/audio/voice_lifecycle.cpp \
	$(CORE)/audio/voice_binding.cpp \
	$(CORE)/util/math_lut.cpp \
	$(CORE)/util/random.cpp \
	$(CORE)/util/json.cpp \
	$(CORE)/util/sha256.cpp \
	$(CORE)/formats/bin_reader.cpp \
	$(CORE)/formats/palette.cpp \
	$(CORE)/formats/texture.cpp \
	$(CORE)/formats/texture_archive.cpp \
	$(CORE)/formats/dti.cpp \
	$(CORE)/formats/mto.cpp \
	$(CORE)/formats/arena.cpp \
	$(CORE)/formats/model.cpp \
	$(CORE)/formats/model_animation.cpp \
	$(CORE)/formats/cmi.cpp \
	$(CORE)/formats/cmi_beta.cpp \
	$(CORE)/formats/script_decoder.cpp \
	$(CORE)/formats/script_decoder_beta.cpp \
	$(CORE)/formats/script_opcodes.cpp \
	$(CORE)/formats/file_system.cpp \
	$(CORE)/formats/case_insensitive_path.cpp \
	$(CORE)/formats/local_paths.cpp \
	$(CORE)/formats/mdk_data.cpp \
	$(CORE)/formats/mve.cpp \
	$(CORE)/formats/sni.cpp \
	$(CORE)/formats/wav.cpp \
	$(CORE)/formats/sprite_animation.cpp \
	$(CORE)/formats/bni.cpp \
	$(CORE)/formats/fti.cpp \
	$(CORE)/formats/font.cpp \
	$(CORE)/game/font_view.cpp \
	$(CORE)/game/bomber_overlay.cpp \
	$(CORE)/game/messages.cpp \
	$(CORE)/game/level_data.cpp \
	$(CORE)/game/arena_stream.cpp \
	$(CORE)/game/level_flow.cpp \
	$(CORE)/game/arena_space.cpp \
	$(CORE)/game/solids.cpp \
	$(CORE)/game/bsp.cpp \
	$(CORE)/game/camera_roll.cpp \
	$(CORE)/game/cutscene_camera.cpp \
	$(CORE)/game/follow_camera.cpp \
	$(CORE)/game/frame_blend.cpp \
	$(CORE)/game/game_flow.cpp \
	$(CORE)/game/game_state.cpp \
	$(CORE)/game/save_games.cpp \
	$(CORE)/game/save_prompt.cpp \
	$(CORE)/game/viewer_saves.cpp \
	$(CORE)/game/kurt.cpp \
	$(CORE)/game/object_view.cpp \
	$(CORE)/game/effects_view.cpp \
	$(CORE)/game/mod_info.cpp \
	$(CORE)/game/mod_models.cpp \
	$(CORE)/game/part_fit.cpp \
	$(CORE)/formats/glb.cpp \
	$(CORE)/formats/png.cpp \
	$(CORE)/game/player.cpp \
	$(CORE)/game/player_ledge.cpp \
	$(CORE)/game/player_sniper.cpp \
	$(CORE)/game/scope.cpp \
	$(CORE)/render/arena_scene.cpp \
	$(CORE)/render/camera_builder.cpp \
	$(CORE)/render/depth_pull.cpp \
	$(CORE)/render/material_resolver.cpp \
	$(CORE)/scripts/object.cpp \
	$(CORE)/scripts/animated_textures.cpp \
	$(CORE)/scripts/effects.cpp \
	$(CORE)/scripts/fans.cpp \
	$(CORE)/scripts/pose_cache.cpp \
	$(CORE)/scripts/script_vm.cpp \
	$(CORE)/scripts/script_runtime.cpp \
	$(CORE)/scripts/sniper_rounds.cpp \
	$(CORE)/scripts/air_strike.cpp \
	$(CORE)/scripts/strike_scene.cpp \
	$(CORE)/scripts/bullet_holes.cpp \
	$(CORE)/util/system_numerics_inverse.cpp \
	$(CORE)/scripts/items.cpp \
	$(CORE)/scripts/script_snapshot.cpp \
	$(CORE)/scripts/end_level.cpp \
	$(CORE)/scripts/hazards.cpp \
	$(CORE)/scripts/platforms.cpp \
	$(CORE)/scripts/rides.cpp \
	$(CORE)/scripts/snowboard.cpp \
	$(CORE)/scripts/bomber.cpp \
	$(CORE)/scripts/updraft_wind.cpp \
	$(CORE)/scripts/debris.cpp \
	$(CORE)/stream/tube_basis.cpp \
	$(CORE)/stream/stream_tube.cpp \
	$(CORE)/stream/stream_flight.cpp

# tick1 is the golden-state binary; vm_tests is the synthetic per-opcode
# harness (the C++ twin of tests/Mdk.Game.Tests/ScriptVmTests.cs). Both link
# the same core objects: a source that lands on one target only is a silently
# different game, so the shared list stays single-sourced here.
SOURCES := \
	tick1.cpp \
	stream_dump.cpp \
	skydive_dump.cpp \
	save_dump.cpp \
	$(CORE)/fall/fall_sim.cpp \
	$(CORE)/fall/fall_missile.cpp \
	$(CORE)/fall/fall_palette.cpp \
	$(CORE)/fall/fall_ground.cpp \
	$(CORE)/fall/fall_layout.cpp \
	$(CORE)/formats/fall3d.cpp \
	$(CORE)/game/sniper_overlay.cpp \
	$(CORE)/game/sniper_view.cpp \
	$(CORE)/formats/sniper_screen.cpp \
	$(COMMON_SOURCES)

SOURCES_VM := \
	vm_tests.cpp \
	audio_sink_tests.cpp \
	audio_stream_tests.cpp \
	voice_binding_tests.cpp \
	archive_guard_tests.cpp \
	flow_tests.cpp \
	menu_tests.cpp \
	cheats_tests.cpp \
	$(CORE)/game/cheats.cpp \
	$(CORE)/game/cheats_menu.cpp \
	movie_tests.cpp \
	$(CORE)/formats/flc.cpp \
	$(CORE)/game/intro_splash.cpp \
	$(CORE)/game/video_player.cpp \
	$(CORE)/game/end_movie.cpp \
	$(CORE)/game/game_settings.cpp \
	$(CORE)/game/screen_view.cpp \
	$(CORE)/game/menu_items.cpp \
	$(CORE)/game/main_menu.cpp \
	$(CORE)/game/pause_menu.cpp \
	$(CORE)/game/briefing.cpp \
	$(CORE)/game/stats_screen.cpp \
	$(CORE)/game/heads_view.cpp \
	$(CORE)/game/slideshow.cpp \
	$(CORE)/formats/gif.cpp \
	stats_tests.cpp \
	$(CORE)/game/enhanced_menu.cpp \
	$(CORE)/game/enhanced_config.cpp \
	save_tests.cpp \
	layer_tests.cpp \
	mve_audio_tests.cpp \
	sprite_tests.cpp \
	kurt_stream_tests.cpp \
	system_numerics_tests.cpp \
	pica_matrix_tests.cpp \
	input_3ds_tests.cpp \
	heap_split_tests.cpp \
	sky_dome_tests.cpp \
	$(CORE)/render/sky_dome.cpp \
	$(3DS)/render/pica_matrix.cpp \
	$(CORE)/render/sun_shadow.cpp \
	$(3DS)/platform/input_3ds.cpp \
	$(3DS)/platform/controls_3ds.cpp \
	$(3DS)/platform/heap_split_policy.cpp \
	$(CORE)/game/kurt_sprite.cpp \
	$(CORE)/formats/kurt_pack.cpp \
	$(CORE)/game/mod_images.cpp \
	$(CORE)/game/hd_key.cpp \
	$(COMMON_SOURCES)

OBJECTS := $(patsubst %.cpp,$(BUILD)/%.o,$(notdir $(SOURCES)))
OBJECTS_VM := $(patsubst %.cpp,$(BUILD)/%.o,$(notdir $(SOURCES_VM)))
VPATH := $(sort $(dir $(SOURCES) $(SOURCES_VM)))

.PHONY: all bin test test_crossing clean

all: test

bin: $(BUILD)/tick1 $(BUILD)/vm_tests

# The doorway-crossing cases on their own — the acceptance command of the ledger row
# 3ds.collision-doors. It runs the SAME two checks the `test` recipe runs (they are
# inside `tick1`'s own case list, not a separate binary), so this target cannot pass
# while the full suite fails them. The gate suite runs THIS target, not the whole
# suite: PARITY-SCRIPTS-CROSSING in tools/runner/gates_core_parity.py builds and runs
# it from tools/check.sh, which is what stops the row rotting the way tests/formats
# did (review-1 of run-20261110-230107 F2). Additive: `make -C tests/scripts` still
# means what the `core.goldens` row's recorded acceptance means, with these two cases
# added to it.
test_crossing: $(BUILD)/tick1
	@$(BUILD)/tick1 crossing

test: $(BUILD)/tick1 $(BUILD)/vm_tests
	@$(BUILD)/vm_tests
	@sh sprites_refusal_test.sh "$(CURDIR)/$(BUILD)/tick1"
	@sh c3d_frame_test.sh
	@sh ndsp_sink_test.sh
	@sh ubsan_decode_test.sh
	@sh main_3ds_flow_test.sh
	@sh frontend_solid_pin_test.sh
	@sh linux_refusal_report_test.sh
	@sh linux_arg_parse_test.sh
	@sh level_chain_test.sh
	@sh menu_frontend_test.sh
	@sh stream_screen_test.sh
	@sh fall_frontend_test.sh
	@sh panel_geom_test.sh
	@bash emurun_harness_test.sh
	@bash frame_gate_test.sh
	@$(BUILD)/tick1

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: %.cpp | $(BUILD)
	@echo "CXX  $<"
	@$(CXX) $(CXXFLAGS) -MMD -MP -MF $(BUILD)/$*.d -c $< -o $@

$(BUILD)/tick1: $(OBJECTS)
	@echo "LINK $@"
	@$(CXX) $(LDFLAGS) $(OBJECTS) -lz -o $@

$(BUILD)/vm_tests: $(OBJECTS_VM)
	@echo "LINK $@"
	@$(CXX) $(LDFLAGS) $(OBJECTS_VM) -lz -o $@

# INCLUDE BOTH OBJECT LISTS. `vm_tests` is built from OBJECTS_VM, and its own
# .d is only the link-time one; the per-file dependency files of OBJECTS_VM were
# left out, so a HEADER edit did NOT rebuild a vm_tests-only object and the gate
# reported PASS against a stale binary. The reproduced form is the header: with
# the old line, `touch ../../mdk_3ds/src/render/pica_matrix.h` and rebuild gave 0
# `CXX` lines and `PASS PicaMatrix`, and inverting `pica::texelBytes` in that
# header stayed green until the .o was touched by hand. (Batch 120's record also
# named "a row added to pica_matrix_tests.cpp"; review-6 F4 was right that that
# half had no mechanism here -- a source edit always rebuilt its own object, even
# under the old line, measured the same way. The negative control that came back
# green was the header inversion, which is what this line fixes.) No second
# staleness path is known: the `notdir` object names do not collide. Measured from
# `make -p`: SOURCES holds 52 paths and SOURCES_VM 64, and every `notdir` basename
# maps to exactly ONE source path -- 51 basenames are shared between the two lists
# and resolve to the same file, and 0 basenames map to two different sources (65
# distinct basenames over 65 distinct paths). That is the property a VPATH collision
# would break, and it holds; batch 121's "53 and 65 objects, 0 duplicate basenames"
# was wrong on both counts and stated a uniqueness no one can check, because 51
# basenames ARE duplicated across the two lists (review-7 F3).
-include $(OBJECTS:.o=.d) $(OBJECTS_VM:.o=.d) $(BUILD)/vm_tests.d

clean:
	@rm -rf $(BUILD)
