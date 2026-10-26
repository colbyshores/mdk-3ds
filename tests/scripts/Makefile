# Makefile — the scripts' end-to-end checks (the level-3 tick-1 table).
#
# A HOST build, like tests/formats: it exercises the shared code against the
# real game data. The source list is explicit for the same reason.

CORE := ../../mdk_core/src
BUILD := build
CXX ?= g++
CXXFLAGS := -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter \
            -fno-strict-aliasing -I$(CORE) -I. -I../formats
LDFLAGS :=

SOURCES := \
	tick1.cpp \
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
	$(CORE)/formats/fti.cpp \
	$(CORE)/game/level_data.cpp \
	$(CORE)/game/arena_space.cpp \
	$(CORE)/game/bsp.cpp \
	$(CORE)/game/kurt.cpp \
	$(CORE)/render/arena_scene.cpp \
	$(CORE)/render/material_resolver.cpp \
	$(CORE)/scripts/object.cpp \
	$(CORE)/scripts/script_vm.cpp \
	$(CORE)/scripts/script_runtime.cpp \
	$(CORE)/scripts/debris.cpp

OBJECTS := $(patsubst %.cpp,$(BUILD)/%.o,$(notdir $(SOURCES)))
VPATH := $(sort $(dir $(SOURCES)))

.PHONY: all bin test clean

all: test

bin: $(BUILD)/tick1

test: $(BUILD)/tick1
	@$(BUILD)/tick1

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: %.cpp | $(BUILD)
	@echo "CXX  $<"
	@$(CXX) $(CXXFLAGS) -MMD -MP -MF $(BUILD)/$*.d -c $< -o $@

$(BUILD)/tick1: $(OBJECTS)
	@echo "LINK $@"
	@$(CXX) $(LDFLAGS) $(OBJECTS) -o $@

-include $(OBJECTS:.o=.d)

clean:
	@rm -rf $(BUILD)
