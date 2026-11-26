# Makefile -- the host exporter behind mdk_3ds/tools/arena_etc1_convert.py.
#
#   make -C mdk_3ds/tools/arena_etc1 BUILD=<dir>
#
# A HOST build of mdk_core's own parsers (the same sources tests/formats links
# for the level and arena formats, and the same list kurt_etc1/ builds), so the
# textures are decoded, zero-ruled and keyed by the code the 3DS runs. Not part
# of the 3DS build.

CORE  := ../../../mdk_core/src
TESTS := ../../../tests/formats
BUILD ?= build
CXX   ?= g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing \
            -I$(CORE) -I$(TESTS)

SOURCES := \
	arena_tex_export.cpp \
	$(CORE)/util/sha256.cpp \
	$(CORE)/util/math_lut.cpp \
	$(CORE)/formats/bin_reader.cpp \
	$(CORE)/formats/palette.cpp \
	$(CORE)/formats/texture.cpp \
	$(CORE)/formats/texture_archive.cpp \
	$(CORE)/formats/dti.cpp \
	$(CORE)/formats/mto.cpp \
	$(CORE)/formats/arena.cpp \
	$(CORE)/formats/model.cpp \
	$(CORE)/formats/model_animation.cpp \
	$(CORE)/formats/file_system.cpp \
	$(CORE)/formats/case_insensitive_path.cpp \
	$(CORE)/formats/local_paths.cpp \
	$(CORE)/formats/mdk_data.cpp \
	$(CORE)/formats/sni.cpp \
	$(CORE)/formats/bni.cpp \
	$(CORE)/formats/sprite_animation.cpp \
	$(CORE)/formats/fti.cpp \
	$(CORE)/scripts/pose_cache.cpp \
	$(CORE)/game/level_data.cpp \
	$(CORE)/game/hd_key.cpp \
	$(CORE)/render/sky_dome.cpp

OBJECTS := $(patsubst %.cpp,$(BUILD)/%.o,$(notdir $(SOURCES)))
VPATH := $(sort $(dir $(SOURCES)))

.PHONY: all clean
all: $(BUILD)/arena_tex_export

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: %.cpp | $(BUILD)
	@$(CXX) $(CXXFLAGS) -MMD -MP -MF $(BUILD)/$*.d -c $< -o $@

$(BUILD)/arena_tex_export: $(OBJECTS)
	@$(CXX) $(OBJECTS) -lz -o $@

-include $(OBJECTS:.o=.d)

clean:
	@rm -rf $(BUILD)
