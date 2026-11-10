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
	$(CORE)/util/math_lut.cpp \
	$(CORE)/util/random.cpp \
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
	$(CORE)/game/level_data.cpp \
	$(CORE)/game/level_flow.cpp \
	$(CORE)/game/arena_space.cpp \
	$(CORE)/game/bsp.cpp \
	$(CORE)/game/camera_roll.cpp \
	$(CORE)/game/follow_camera.cpp \
	$(CORE)/game/frame_blend.cpp \
	$(CORE)/game/game_flow.cpp \
	$(CORE)/game/game_state.cpp \
	$(CORE)/game/kurt.cpp \
	$(CORE)/game/object_view.cpp \
	$(CORE)/game/player.cpp \
	$(CORE)/render/arena_scene.cpp \
	$(CORE)/render/camera_builder.cpp \
	$(CORE)/render/depth_pull.cpp \
	$(CORE)/render/material_resolver.cpp \
	$(CORE)/scripts/object.cpp \
	$(CORE)/scripts/script_vm.cpp \
	$(CORE)/scripts/script_runtime.cpp \
	$(CORE)/scripts/debris.cpp

# tick1 is the golden-state binary; vm_tests is the synthetic per-opcode
# harness (the C++ twin of tests/Mdk.Game.Tests/ScriptVmTests.cs). Both link
# the same core objects: a source that lands on one target only is a silently
# different game, so the shared list stays single-sourced here.
SOURCES := \
	tick1.cpp \
	$(COMMON_SOURCES)

SOURCES_VM := \
	vm_tests.cpp \
	audio_sink_tests.cpp \
	archive_guard_tests.cpp \
	flow_tests.cpp \
	layer_tests.cpp \
	mve_audio_tests.cpp \
	sprite_tests.cpp \
	system_numerics_tests.cpp \
	pica_matrix_tests.cpp \
	input_3ds_tests.cpp \
	$(LINUX)/render/system_numerics_inverse.cpp \
	$(3DS)/render/pica_matrix.cpp \
	$(3DS)/platform/input_3ds.cpp \
	$(COMMON_SOURCES)

OBJECTS := $(patsubst %.cpp,$(BUILD)/%.o,$(notdir $(SOURCES)))
OBJECTS_VM := $(patsubst %.cpp,$(BUILD)/%.o,$(notdir $(SOURCES_VM)))
VPATH := $(sort $(dir $(SOURCES) $(SOURCES_VM)))

.PHONY: all bin test clean

all: test

bin: $(BUILD)/tick1 $(BUILD)/vm_tests

test: $(BUILD)/tick1 $(BUILD)/vm_tests
	@$(BUILD)/vm_tests
	@sh sprites_refusal_test.sh "$(CURDIR)/$(BUILD)/tick1"
	@sh c3d_frame_test.sh
	@sh ndsp_sink_test.sh
	@sh ubsan_decode_test.sh
	@sh main_3ds_flow_test.sh
	@sh frontend_solid_pin_test.sh
	@sh panel_geom_test.sh
	@bash emurun_harness_test.sh
	@$(BUILD)/tick1

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: %.cpp | $(BUILD)
	@echo "CXX  $<"
	@$(CXX) $(CXXFLAGS) -MMD -MP -MF $(BUILD)/$*.d -c $< -o $@

$(BUILD)/tick1: $(OBJECTS)
	@echo "LINK $@"
	@$(CXX) $(LDFLAGS) $(OBJECTS) -o $@

$(BUILD)/vm_tests: $(OBJECTS_VM)
	@echo "LINK $@"
	@$(CXX) $(LDFLAGS) $(OBJECTS_VM) -o $@

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
