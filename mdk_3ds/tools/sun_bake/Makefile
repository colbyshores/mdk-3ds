# Makefile -- the host baker behind mdk_3ds/tools/sun_bake.sh.
#
#   make -C mdk_3ds/tools/sun_bake BUILD=<dir>
#
# A HOST build of mdk_core's own parsers and arena build (the sources
# tests/formats links), plus the two pure-arithmetic pieces of the 3DS tree the
# bake shares with the runtime: render/pica_matrix (castsSunShadow,
# textureSourceHasHoles, the tiled layout) and render/sun_bake (the file).
# Not part of the 3DS build.

CORE  := ../../../mdk_core/src
3DS   := ../../src
TESTS := ../../../tests/formats
BUILD ?= build
CXX   ?= g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing \
            -ffp-contract=off -I$(CORE) -I$(3DS) -I$(TESTS) -I.

SOURCES := \
	sun_bake_level.cpp \
	sun_baker.cpp \
	$(3DS)/render/sun_bake.cpp \
	$(3DS)/render/pica_matrix.cpp \
	$(CORE)/render/sun_shadow.cpp \
	$(CORE)/render/arena_scene.cpp \
	$(CORE)/render/material_resolver.cpp \
	$(CORE)/game/level_light.cpp \
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
	$(CORE)/formats/sprite_animation.cpp \
	$(CORE)/formats/fti.cpp \
	$(CORE)/scripts/pose_cache.cpp \
	$(CORE)/game/level_data.cpp \
	$(CORE)/game/arena_stream.cpp

OBJECTS := $(patsubst %.cpp,$(BUILD)/%.o,$(notdir $(SOURCES)))
VPATH := $(sort $(dir $(SOURCES)))

.PHONY: all clean
all: $(BUILD)/sun_bake_level

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: %.cpp | $(BUILD)
	@$(CXX) $(CXXFLAGS) -MMD -MP -MF $(BUILD)/$*.d -c $< -o $@

$(BUILD)/sun_bake_level: $(OBJECTS)
	@$(CXX) $(OBJECTS) -lz -o $@

-include $(OBJECTS:.o=.d)

clean:
	@rm -rf $(BUILD)
