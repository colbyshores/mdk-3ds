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
	lighting_tests.cpp \
	math_lut_tests.cpp \
	random_tests.cpp \
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
	$(CORE)/formats/file_system.cpp \
	$(CORE)/formats/case_insensitive_path.cpp \
	$(CORE)/formats/local_paths.cpp \
	$(CORE)/formats/mdk_data.cpp \
	$(CORE)/formats/sni.cpp \
	$(CORE)/formats/sprite_animation.cpp \
	$(CORE)/formats/fti.cpp \
	$(CORE)/scripts/pose_cache.cpp \
	$(CORE)/game/level_data.cpp \
	$(CORE)/game/level_light.cpp

# scripts/pose_cache.cpp is here because game/level_data.cpp calls
# scripts::forgetPoseCaches(), and a target links whatever a call resolves to.
# It is the ONE scripts translation unit this list carries, and it is the only
# one it can carry: scripts/object.cpp -- where the definition used to live --
# needs game/frame_blend.cpp and ScriptRuntime, and the runtime needs the BSP,
# the mixer, Kurt and the player. Linking that would make this the whole game
# rather than a format test. The split is documented in pose_cache.cpp.

# The formats the port has landed so far. The list grows as the batches
# land; a source that is missing from BOTH this list and the tree is a
# build error, not a silent omission.
OBJECTS := $(patsubst %.cpp,$(BUILD)/%.o,$(notdir $(SOURCES)))
VPATH := $(sort $(dir $(SOURCES)))

# The link graph is the thing that rotted for 25 batches, and the tempting wrong
# repair is to keep adding game sources to SOURCES until the linker stops
# complaining. That links, and stops being a format test. So the boundary is
# asserted here rather than left to taste: no script-VM symbol may appear in this
# binary. scripts/pose_cache.cpp is the ONE scripts translation unit this list
# carries, and the only one it may carry. The pattern is checked against the
# LINKED binary, so it catches the symbol that arrived through a chain of calls
# and not just one added to SOURCES; tests/scripts/build/tick1, which does link
# the VM, is the positive control for every name in it.
VM_PATTERN := mdk::ScriptRuntime|mdk::Bsp::|mdk::Kurt::|mdk::Player::|mdk::audio::SoundMixer::|mdk::ArenaSpace::

# HOW THE GUARD READS THE BINARY, and why it is spelled this way. It reads `nm`
# output, so the guard has a failure mode the link graph does not: `nm` itself.
# `nm -C '$@' | grep -qE ...` reports "no match" -- the clean answer -- when `nm`
# is not on PATH, when it fails on the file, and when it prints nothing, because
# a pipeline's status is its last command's and grep never saw a line. That is a
# gate that skips on missing input, which is a pass generator: the same mistake
# ralph-pi.sh refuses to make about its own reviewer, and the reason batch 95's
# sanitizer gate runs two sanitizers instead of one. So the symbol dump is taken
# to a file, its exit status is checked, an empty dump is a failure, and a host
# with no binutils stops the target instead of clearing it. Review-15 R15-8.
SYMS := $(BUILD)/format_tests.symbols

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
	@if ! command -v nm >/dev/null 2>&1; then \
	    echo "LINK GRAPH: nm is not on PATH, so the link-graph guard cannot run." >&2; \
	    echo "  refusing to report $@ clean on a check that did not execute." >&2; \
	    rm -f '$@'; exit 1; \
	fi
	@nm -C '$@' > '$(SYMS)' 2>&1; status=$$?; \
	if [ $$status -ne 0 ] || [ ! -s '$(SYMS)' ]; then \
	    echo "LINK GRAPH: nm gave no symbol dump for $@ (exit $$status) -- nothing to check." >&2; \
	    sed 's/^/    /' '$(SYMS)' >&2; \
	    rm -f '$@' '$(SYMS)'; exit 1; \
	fi
	@if grep -qE '$(VM_PATTERN)' '$(SYMS)'; then \
	    echo "LINK GRAPH: $@ links the script VM -- a format test that links the game is not a format test." >&2; \
	    echo "  what it pulled in:" >&2; \
	    grep -E '$(VM_PATTERN)' '$(SYMS)' | sed 's/^/    /' | head -5 >&2; \
	    echo "  the repair is NOT to add scripts/object.cpp to SOURCES: see scripts/pose_cache.cpp." >&2; \
	    rm -f '$(SYMS)'; rm -f '$@'; exit 1; \
	fi
	@rm -f '$(SYMS)'

-include $(OBJECTS:.o=.d)

clean:
	@rm -rf $(BUILD)
