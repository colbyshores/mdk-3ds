#
# mdk_3ds/Makefile — MDK Nintendo 3DS port (devkitPro / devkitARM).
#
# Usage (from the repo root):
#   make -C mdk_3ds            # release build (Citro3D / PICA200)
#   make -C mdk_3ds DEBUG=1    # debug build (__3DS_DEBUG__ defined)
#   make -C mdk_3ds cia        # build the .cia with the MDK data in RomFS
#   make -C mdk_3ds clean
#
# Sources come from TWO trees: ../mdk_core/src (the platform-agnostic game --
# formats, logic, the script VM, the collision, the audio mixer, the renderer
# seam) and ./src (this target's platform layer: main_3ds, the Citro3D
# backend, the ndsp audio sink). The Linux tree lives in ../mdk_linux and is
# never globbed here, so the old EXCLUDE_SRC list is empty by construction.
#
# Modeled on ../t2k/t2k_3ds/Makefile and ../forsaken/Makefile.3ds.
#
# `make clean` is MANDATORY when flipping any -D flag — this Makefile does
# not track flag changes.
#

DEBUG ?= 0

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM. export DEVKITARM=<path to>/devkitARM")
endif
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO. export DEVKITPRO=<path to>/devkitpro")
endif

include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# Paths
#---------------------------------------------------------------------------------
SRCDIR   := $(CURDIR)/src
CORE     := $(abspath $(CURDIR)/../mdk_core)
CORESRC  := $(CORE)/src
TARGET   := mdk
BUILD    := build
PORTLIBS := $(DEVKITPRO)/portlibs/3ds

# The CIA's inputs. These are defined HERE, before any rule that names them:
# a `:=` variable is expanded when its line is read, so a rule above the
# definition would see an EMPTY value -- and an empty prerequisite SILENTLY
# DROPS the dependency. That is how the .3dsx came to be built with no
# RomFS and no SMDH while make cheerfully reported it "up to date".
CIA_DIR     := $(CURDIR)/cia
CIA_ROMFS   := $(BUILD)/romfs
CIA_SMDH    := $(BUILD)/$(TARGET).smdh

# The default goal. Without this, make takes the FIRST target in the file,
# which is the PICA shader's .shbin -- so a bare `make` would build a shader
# and stop, and the build would look like it "succeeded" while producing no
# .3dsx at all. The first target in a Makefile is the default goal; this
# makes it explicit rather than positional.
.DEFAULT_GOAL := $(TARGET).3dsx

#---------------------------------------------------------------------------------
# Source globbing
#
# THE WILDCARD IS PER-DEPTH, and that is a real trap: it is a list of explicit
# globs, not a recursive walk, so a source in a NEW nesting level is silently
# dropped -- the 3DS build stays green and simply links a different game.
# mdk_core/src/game/collision/ and mdk_core/src/game/kurt/ are the second
# level; mdk_core/src/scripts/ is the first. Add a glob here before adding a
# directory there.
#
# NB `notdir` means BASENAMES MUST BE UNIQUE across the whole subtree -- two
# files called renderer.cpp in different directories would collapse into one
# object and one of them would never be compiled.
#---------------------------------------------------------------------------------
EXCLUDE_SRC :=

SRC_GLOBS := $(wildcard $(CORESRC)/*.cpp) \
             $(wildcard $(CORESRC)/*/*.cpp) \
             $(wildcard $(CORESRC)/*/*/*.cpp) \
             $(wildcard $(SRCDIR)/*.cpp) \
             $(wildcard $(SRCDIR)/*/*.cpp) \
             $(wildcard $(SRCDIR)/*/*/*.cpp)

ALL_SRC := $(notdir $(SRC_GLOBS))
CPPFILES := $(filter-out $(EXCLUDE_SRC),$(ALL_SRC))
OFILES   := $(addprefix $(BUILD)/,$(CPPFILES:.cpp=.o))

# Let make find the sources across the src/ subtree.
VPATH := $(sort $(dir $(SRC_GLOBS)))

#---------------------------------------------------------------------------------
# Flags — ARMv6K (ARM11), hard float, VFP.
#
# See AGENTS.md R10. The flag set is the devkitPro application template's
# baseline, and it MUST reach the compile and link commands -- a literal ARCH
# line alone is not verification.
#---------------------------------------------------------------------------------
ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -mfpu=vfp

# -mtp=soft is REQUIRED: it selects runtime-helper access to the thread-local
# storage pointer (__aeabi_read_tp). It is separate from the floating-point
# ABI; do not remove it as a supposed float optimisation.
#
# -fno-strict-aliasing is REQUIRED in BOTH builds (AGENTS.md R10): the format
# parsers read binary data through memcpy into typed locals, and the ARM11 is
# unforgiving about aliasing assumptions a desktop optimiser may take.
#
# -fno-math-errno lets GCC optimise e.g. sqrtf without preserving errno
# reporting. It is NOT what enables hardware floating-point division. Code
# must not depend on math functions setting errno under this policy.
#
# NO -ffast-math and NO -Ofast. The fidelity-sensitive arithmetic and the
# narrow per-object -fsingle-precision-constant scope (bottom of this file)
# depend on IEEE semantics.
DEFINES := -D__3DS__ -DARM -DPLATFORM_3DS -DRENDERER_C3D
ifeq ($(DEBUG),1)
DEFINES += -D__3DS_DEBUG__
endif

COMMONFLAGS := $(ARCH) -O2 -ffunction-sections -fdata-sections \
               -fno-math-errno -fno-strict-aliasing $(DEFINES)

INCLUDE := -I$(CORESRC) \
	-I$(SRCDIR) \
	-I$(BUILD) \
	-I$(DEVKITPRO)/libctru/include \
	-I$(PORTLIBS)/include

CXXFLAGS := $(COMMONFLAGS) $(INCLUDE) -std=gnu++17 -fno-exceptions -fno-rtti \
	-Wall -Wextra -Wno-unused-parameter

LDFLAGS := $(ARCH) -specs=3dsx.specs -Wl,-Map,$(TARGET).map \
	-L$(DEVKITPRO)/libctru/lib -L$(PORTLIBS)/lib
LIBS := -lcitro3d -lctru -lm -lz

#---------------------------------------------------------------------------------
# PICA shaders — picasso assembles .v.pica into a DVLB, bin2s embeds it.
#
# Each shader gets THREE explicit rules. There is deliberately NO %.shbin.o
# pattern rule: a pattern rule cannot express the "one .shbin -> one .s ->
# one .o" chain without a second pattern rule, and the chained version has a
# stale-header failure mode (a .s left over from an older .shbin).
#---------------------------------------------------------------------------------
PICASSO := $(DEVKITPRO)/tools/bin/picasso
BIN2S   := $(DEVKITPRO)/tools/bin/bin2s

SHADER_PICA  := $(CURDIR)/shaders/mdk_c3d.v.pica
SHADER_SHBIN := $(BUILD)/mdk_c3d.shbin
SHADER_HDR   := $(BUILD)/mdk_c3d_shbin.h
SHADER_OBJ   := $(BUILD)/mdk_c3d_shbin.o

$(SHADER_SHBIN): $(SHADER_PICA) | $(BUILD)
	@echo "PICA $<"
	@$(PICASSO) -o $@ $<

# &: (grouped target, GNU Make 4.3+) is load-bearing here, NOT stylistic: with
# a plain ':', two targets sharing one recipe are not atomic, so `make -j` can
# schedule this recipe TWICE concurrently -- both instances race to write the
# same .s in $(BUILD), and the loser's truncated write can pair a corrupt .s
# with a header claiming the old (correct) _size. Reproduced on GNU Make
# 4.3. `make clean` if you ever see a DVLB parse failure that a serial
# rebuild fixes.
$(SHADER_HDR) $(SHADER_OBJ) &: $(SHADER_SHBIN)
	@echo "SHBN $(SHADER_SHBIN)"
	@cd $(BUILD) && $(BIN2S) -H mdk_c3d_shbin.h mdk_c3d.shbin > mdk_c3d_shbin.s
	@$(CC) $(ARCH) -x assembler-with-cpp -c $(BUILD)/mdk_c3d_shbin.s -o $(SHADER_OBJ)

#---------------------------------------------------------------------------------
# The build
#---------------------------------------------------------------------------------
$(BUILD):
	@mkdir -p $@

$(BUILD)/%.o: %.cpp | $(BUILD)
	@echo "CXX  $<"
	@$(CXX) -MMD -MP -MF $(BUILD)/$*.d $(CXXFLAGS) -c $< -o $@

# renderer_c3d.cpp includes the generated shader header.
$(BUILD)/renderer_c3d.o: $(SHADER_HDR)

$(TARGET).elf: $(OFILES) $(SHADER_OBJ)
	@echo "LINK $@"
	@$(CXX) $(LDFLAGS) $(OFILES) $(SHADER_OBJ) $(LIBS) -o $@

# The .3dsx carries the SMDH and the RomFS. The SMDH is what the HOME menu
# shows; the RomFS is the game's data. Both are embedded, so the .3dsx is
# self-contained and needs no separate data directory at run time.
$(TARGET).3dsx: $(TARGET).elf $(CIA_SMDH) $(CIA_ROMFS)
	@echo "3DSX $@"
	@3dsxtool --smdh=$(CIA_SMDH) --romfs=$(CIA_ROMFS) $< $@

-include $(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# THE TITLE'S IDENTITY ON THE HOME MENU IS THREE AUTHORED STRINGS.
#
# smdhtool takes a short description, a long description and a publisher, and
# the HOME menu shows the LONG one under the icon when you hover it.
#
# The publisher is NOT the engine's internal source name -- it is the studio
# that made the original game, which is what a player expects to see on a
# homebrew screen. AGENTS.md §9 requires this to be enforced, not assumed:
# tools/runner/gates_smdh.py re-reads the SMDH out of the BUILT .cia and fails
# if any populated language slot disagrees.
#---------------------------------------------------------------------------------
APP_TITLE        := MDK
APP_DESCRIPTION  := Metal Dungeon
APP_AUTHOR       := Shiny Entertainment
# UniqueId is the title's identity to the console: change it and an install
# becomes a SECOND, separate title rather than an update of the first (the old
# one keeps its save data and stays on the menu). 0xF7A4D is in the homebrew
# range and is arbitrary but FIXED -- pick once, never move.
APP_UNIQUE_ID    := 0xF7A4D

.PHONY: FORCE
FORCE:

# THE SMDH DEPENDS ON THE NAME, NOT JUST ON THE ART. Its inputs are two PNGs
# and three STRINGS, and make cannot see a string -- so `make cia APP_TITLE=...`
# happily reused an SMDH built under the old name and shipped a title calling
# itself the wrong thing, with every check downstream passing because the bytes
# were internally consistent. The stamp makes the strings a FILE, which make
# can see. FORCE, so the stamp's recipe is re-evaluated every build; `cmp` then
# leaves the file's mtime ALONE unless the strings actually changed, so the
# SMDH rebuilds when the name moves and not otherwise.
$(BUILD)/smdh.stamp: FORCE | $(BUILD)
	@printf '%s\n' "$(APP_TITLE)|$(APP_DESCRIPTION)|$(APP_AUTHOR)" > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp

$(CIA_SMDH): $(CIA_DIR)/icon.png $(CIA_DIR)/icon_small.png $(BUILD)/smdh.stamp | $(BUILD)
	@echo "SMDH $@"
	@smdhtool --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" \
	          $(CIA_DIR)/icon.png $@ $(CIA_DIR)/icon_small.png

# The RomFS is staged from the user's own copy of the game (../MDK). The
# staging script lower-cases and hard-links the tree, because 3dsxtool does
# not follow symlinks and the 3DS filesystem is case-sensitive while the game's
# data is mixed-case.
.PHONY: $(CIA_ROMFS)
$(CIA_ROMFS): | $(BUILD)
	@echo "RFS  $@"
	@$(CURDIR)/tools/stage_romfs.sh $@

.PHONY: cia
cia: $(TARGET).cia

# Stage the game data into the RomFS tree without building anything else.
# Useful when checking what the build would ship.
.PHONY: data
data: $(CIA_ROMFS)

# The .cia needs makerom, which is NOT part of the devkitPro toolchain as
# installed. tools/build_cia.sh says so plainly and exits non-zero rather
# than producing a .cia that installs and then shows the wrong title.
$(TARGET).cia: $(TARGET).elf $(CIA_SMDH) $(CIA_ROMFS) $(CIA_DIR)/icon.png $(CIA_DIR)/banner.png
	@echo "CIA  $@"
	@$(CURDIR)/tools/build_cia.sh $@ $(TARGET).elf $(CIA_SMDH) $(CIA_ROMFS) \
	    $(CIA_DIR)/icon.png $(CIA_DIR)/banner.png

#---------------------------------------------------------------------------------
# Scoped float-literal discipline.
#
# -fsingle-precision-constant kills the implicit float->double->float
# promotion from unsuffixed FP literals in a hot per-frame builder. It is
# scoped to the objects that need it and is deliberately NOT global: widening
# it retypes every unsuffixed float literal in the tree, which silently
# changes the mechanics-gated DOUBLE math AGENTS.md R4 protects.
#---------------------------------------------------------------------------------
$(BUILD)/bsp.o:        CXXFLAGS += -fsingle-precision-constant
$(BUILD)/script_vm.o:  CXXFLAGS += -fsingle-precision-constant
$(BUILD)/audio_device.o: CXXFLAGS += -fsingle-precision-constant

# FP contraction -- deliberately NOT disabled here, and it would change nothing.
#
# C# never contracts a multiply-add, so `a + b*c` rounds twice there. GCC's ARM
# backend still selects VFPv2's VMLA.F32/VNMLS.F32 for that shape, and
# -ffp-contract does not stop it: with the shipped flags debris.cpp.o is BYTE-
# IDENTICAL under the default, -ffp-contract=off and -ffp-contract=fast (4 MACs
# in Debris::randomSpin each way), and the default here is `fast`, not `off`,
# because -std=gnu++17 is a GNU dialect (`arm-none-eabi-g++ -Q --help=common`,
# devkitARM 15.2.0; the host gcc 13.3 defaults to `fast` too).
#
# The reason the flag has nothing to gate is that VMLA.F32 is NOT a fused MAC:
# VMLA/VMLS/VNMLA/VNMLS round their product to the destination format before
# accumulating, and the single-rounded forms are the VFPv4 VFMA/VFMS this target
# cannot encode. Forming a VMLA is therefore value-preserving, which is why the
# flag does not mention it. Measured four ways on this tree with the shipped
# flags (docs/PROGRESS_LOG.md, "Iteration 87"):
#   * `float t = a*b; return c + t;` -- twice-rounded by construction -- compiles
#     to ONE vmla.f32 under -ffp-contract=off. A fused VMLA could not do that.
#   * the same source at -mfpu=vfpv4 gives vmla.f32 under =off and vfma.f32 under
#     =fast: the flag gates the FUSED form, and only the fused form.
#   * __builtin_fmaf at -mfpu=vfp is `b fmaf`, a libm call, because VFPv2 has no
#     fused instruction to use (the fact docs/OPEN_ITEMS.md records for
#     frame_blend.cpp).
#   * the shipped randomSpin disassembly's own operand grouping, replayed with a
#     rounded product, reproduces 0/72 of the oracle's pinned spin components
#     wrong; replayed as a fused MAC it gets 23/72 wrong on 12 of 18 pieces.
# So the shipped object's MACs compute the same twice-rounded value as the C
# source and as the host harness: there is no VMLA rounding gap to measure and no
# expression to "fix". What IS still unmeasured on Old-3DS hardware is the libm
# leaf -- the shipped sinf/cosf come from newlib, and only glibc's have been
# checked bit-for-bit against MathF.Sin/MathF.Cos. Look there first if a capture
# ever shows a divergence.

clean:
	@echo "CLEAN"
	@rm -rf $(BUILD) $(TARGET).elf $(TARGET).3dsx $(TARGET).cia $(TARGET).map
