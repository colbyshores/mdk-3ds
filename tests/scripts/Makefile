# Makefile — the scripts' end-to-end checks (the level-3 tick-1 table).
#
# A HOST build, like tests/formats: it exercises the shared code against the
# real game data. The source list is explicit for the same reason.

CORE := ../../mdk_core/src
LINUX := ../../mdk_linux/src
# The 3DS platform layer, for the ONE piece of it that is pure arithmetic and
# can be pinned here: render/pica_matrix.{h,cpp}. The rest of that tree includes
# <3ds.h> and cannot be compiled on a host, which is why the matrix math was
# split out of renderer_c3d.cpp in the first place. Last on the include path so
# it can never shadow a core or Linux header.
3DS := ../../mdk_3ds/src
BUILD := build
CXX ?= g++
CXXFLAGS := -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter \
            -fno-strict-aliasing -I$(CORE) -I$(LINUX) -I. -I../formats -I$(3DS)
LDFLAGS :=

COMMON_SOURCES := \
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
	$(CORE)/formats/sni.cpp \
	$(CORE)/formats/wav.cpp \
	$(CORE)/formats/sprite_animation.cpp \
	$(CORE)/formats/bni.cpp \
	$(CORE)/formats/fti.cpp \
	$(CORE)/game/level_data.cpp \
	$(CORE)/game/arena_space.cpp \
	$(CORE)/game/bsp.cpp \
	$(CORE)/game/frame_blend.cpp \
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
	archive_guard_tests.cpp \
	layer_tests.cpp \
	sprite_tests.cpp \
	system_numerics_tests.cpp \
	pica_matrix_tests.cpp \
	$(LINUX)/render/system_numerics_inverse.cpp \
	$(3DS)/render/pica_matrix.cpp \
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

-include $(OBJECTS:.o=.d) $(BUILD)/vm_tests.d

clean:
	@rm -rf $(BUILD)
