# Makefile — the C++ format tests.
#
# The tests are a HOST build: they exercise the shared code, which is the
# same code the 3DS compiles. They are not part of the 3DS build, and they
# are not part of the Linux game build -- a test binary is not a game.
#
# Usage:
#   make -C tests/formats            build and run
#   make -C tests/formats bin        build only
#   make -C tests/formats clean
#
# The formats' sources are listed EXPLICITLY, for the same reason the Linux
# CMakeLists lists them: a source that lands on one target only is a
# SILENTLY DIFFERENT GAME, and here it would be a silently different test.

CORE := ../../mdk_core/src
BUILD := build
CXX ?= g++
CXXFLAGS := -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter \
            -fno-strict-aliasing -I$(CORE) -I.
LDFLAGS :=

SOURCES := \
	main.cpp \
	path_tests.cpp \
	format_tests.cpp \
	math_lut_tests.cpp \
	$(CORE)/util/math_lut.cpp \
	$(CORE)/formats/bin_reader.cpp \
	$(CORE)/formats/palette.cpp \
	$(CORE)/formats/texture.cpp \
	$(CORE)/formats/texture_archive.cpp \
	$(CORE)/formats/dti.cpp \
	$(CORE)/formats/mto.cpp \
	$(CORE)/formats/arena.cpp \
	$(CORE)/formats/file_system.cpp \
	$(CORE)/formats/case_insensitive_path.cpp \
	$(CORE)/formats/local_paths.cpp \
	$(CORE)/formats/mdk_data.cpp

# The formats the port has landed so far. The list grows as the batches
# land; a source that is missing from BOTH this list and the tree is a
# build error, not a silent omission.
OBJECTS := $(patsubst %.cpp,$(BUILD)/%.o,$(notdir $(SOURCES)))
VPATH := $(sort $(dir $(SOURCES)))

.PHONY: all bin test clean

all: test

bin: $(BUILD)/format_tests

test: $(BUILD)/format_tests
	@$(BUILD)/format_tests

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: %.cpp | $(BUILD)
	@echo "CXX  $<"
	@$(CXX) $(CXXFLAGS) -MMD -MP -MF $(BUILD)/$*.d -c $< -o $@

$(BUILD)/format_tests: $(OBJECTS)
	@echo "LINK $@"
	@$(CXX) $(LDFLAGS) $(OBJECTS) -o $@

-include $(OBJECTS:.o=.d)

clean:
	@rm -rf $(BUILD)
