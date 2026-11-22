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
# The autonomous hardware loop (platform/harness_3ds.h): `make AUTOQUIT=90`
# builds the harness variant that quits by itself after 90 seconds and
# chain-loads the netload stub on exit. NEVER set this for a release or
# CIA build (build_cia.sh refuses it): the harness must not ship.
ifdef AUTOQUIT
DEFINES += -DMDK_AUTOQUIT=$(AUTOQUIT)
endif
# `make AUTOQUIT=90 CAPTURE=600` additionally writes ONE framebuffer capture at
# frame 600 to sdmc:/3ds/mdk/hwcap.bmp.log (harness_3ds.h), the picture
# `hw_cycle.sh --capture` pulls. Harness-only, and never for a profiling run:
# the capture stalls a frame on SD. Flipping it needs `make clean` (the
# Makefile does not track -D changes).
ifneq ($(strip $(CAPTURE)),)
ifndef AUTOQUIT
$(error CAPTURE=$(CAPTURE) needs AUTOQUIT: the capture is a harness-build feature)
endif
DEFINES += -DMDK_HW_CAPTURE=$(CAPTURE)
endif
# `make AUTOQUIT=90 MONO=1` draws one eye whatever the 3D slider says (the
# renderer's eyePlan sees slider 0): the stereo-cost A/B on the appliance,
# whose slider is up and out of reach. Harness-only; `make clean` after flipping it.
ifeq ($(strip $(MONO)),1)
ifndef AUTOQUIT
$(error MONO=1 needs AUTOQUIT: the A/B switch is a harness-build feature)
endif
DEFINES += -DMDK_HW_MONO=1
endif
# `make AUTOQUIT=90 LEVEL=<3..8>` starts the run in that level instead of the
# flow's first (7): the per-level hardware baselines of the arena-streaming
# plan. Only main_3ds.cpp reads it, so after changing LEVEL `touch
# mdk_3ds/src/main_3ds.cpp` (the Makefile does not track -D changes).
ifneq ($(strip $(LEVEL)),)
ifndef AUTOQUIT
$(error LEVEL=$(LEVEL) needs AUTOQUIT: the level override is a harness-build feature)
endif
DEFINES += -DMDK_HW_LEVEL=$(LEVEL)
endif
# `make AUTOQUIT=90 FALL=<3..8>` starts the run in the fall before that level, then
# the level (the C#'s --fall; package #17b). Only main_3ds.cpp reads it: touch it
# after changing FALL. FIXED=1 seeds the fall with 1 (the Linux --fixed-step's).
ifneq ($(strip $(FALL)),)
ifndef AUTOQUIT
$(error FALL=$(FALL) needs AUTOQUIT: the fall start is a harness-build feature)
endif
DEFINES += -DMDK_HW_FALL=$(FALL)
endif
# `make AUTOQUIT=90 AT=x,y,z,yaw` puts Kurt there instead of at the level's start
# (the Linux build's and the oracle's --at): a scene past the start arena, for the
# arena-streaming comparisons. All four numbers. Only main_3ds.cpp reads it: touch
# it after changing AT.
ifneq ($(strip $(AT)),)
ifndef AUTOQUIT
$(error AT=$(AT) needs AUTOQUIT: the start override is a harness-build feature)
endif
DEFINES += -DMDK_HW_AT='$(AT)'
endif
# `make AUTOQUIT=90 AT=x,y,z,yaw ARENA=NAME` starts Kurt in ARENA the way a script's
# teleport puts him there (ScriptRuntime::teleportKurt, the C#'s TeleportKurt; the
# C# console's map does the same, Viewer.EnterStartArena): the arena is entered,
# loaded and made solid for Kurt even when no connection reaches it (LEVEL3's HMO_7).
# AT alone only moves his feet, so an arena only a teleport reaches is never loaded
# and he falls. Only main_3ds.cpp reads it: touch it after changing ARENA.
ifneq ($(strip $(ARENA)),)
ifeq ($(strip $(AT)),)
$(error ARENA=$(ARENA) needs AT=x,y,z,yaw: the teleport needs a place in the arena)
endif
DEFINES += -DMDK_HW_ARENA='"$(ARENA)"'
endif
# `make AUTOQUIT=90 WALK='forward+turbo@60-260;turnright@270-307'` drives Kurt by a
# script (platform/harness_3ds.h, game/walk_script.h): `<keys>@<first>-<end>`
# segments, ';' between them, '+' between keys (forward, back, strafeleft,
# straferight, turnleft, turnright, jump, turbo, fire, useitem, itemnext,
# itemprevious, sniper, zoomin, zoomout, down: noclip's descent), the end frame
# exclusive. With FIXED=1 frame f is step f, as on the
# Linux build's --fixed-step --walk=..., where a route is found and checked. The
# script is parsed at boot and a refused one stops the run. main_3ds.cpp and
# harness_3ds.cpp read it: `make clean` after changing WALK. No spaces, no quotes.
ifneq ($(strip $(WALK)),)
ifndef AUTOQUIT
$(error WALK=$(WALK) needs AUTOQUIT: the scripted walk is a harness-build feature)
endif
DEFINES += -DMDK_HW_WALK='"$(WALK)"'
endif
# `make AUTOQUIT=90 LEVEL=7 HDROMFS=1` reads the HD pack from romfs:/hd instead of
# sdmc:/3ds/mdk/hd: stage_romfs.sh hard-links that level's .t3x files and manifest
# lines out of build-hd-pack/ (mdk_3ds/tools/build_hd_pack.sh) into the RomFS, so
# the HD stream can be measured on a console whose SD card has no pack (the
# appliance; the hardware loop pushes only the .3dsx). Only main_3ds.cpp reads it:
# `make clean` after changing HDROMFS.
ifneq ($(strip $(HDROMFS)),)
ifndef AUTOQUIT
$(error HDROMFS=$(HDROMFS) needs AUTOQUIT: the RomFS pack is a harness-build feature)
endif
ifeq ($(strip $(LEVEL)),)
$(error HDROMFS=$(HDROMFS) needs LEVEL=n: only that level's pack is staged)
endif
DEFINES += -DMDK_HW_HDROMFS=1
export MDK_HD_ROMFS_LEVEL := $(LEVEL)
endif
# `make AUTOQUIT=90 MENU=Menu.Accept@2,Menu.Back@4,Menu.Accept@5` presses the
# menus' keys at those seconds of the menus' game time (game/menu_presses.h, the
# C#'s TestPresses spelling: Menu.Up/Down/Left/Right/Accept/Back), for a build
# WITHOUT LEVEL=, which boots into the main menu (menu_screens_3ds.h). The example
# is New Game, the briefing's typing skipped, the briefing closed: the first
# level. Only menu_screens_3ds.cpp reads it: `make clean` after changing MENU.
ifneq ($(strip $(MENU)),)
ifndef AUTOQUIT
$(error MENU=$(MENU) needs AUTOQUIT: the menu presses are a harness-build feature)
endif
DEFINES += -DMDK_HW_MENU='"$(MENU)"'
endif
# THE SET PIECES' AIDS (game/harness_aids.h; the Linux build's --give, --ride, --aim),
# for playthroughs that start at a set piece. The first level's only. A refused value
# is logged ("main: harness aids ... REFUSED") and that aid dropped. Only main_3ds.cpp
# reads them: touch it (or `make clean`) after changing any. No spaces, no quotes.
#   GIVE=SW_BONES[,SW_HOME...]  those pickups in Kurt's inventory at spawn (Kurt::collect,
#                               the items probe's `give:`).
#   RIDE=XE|XD2|XD|XSNOWB|X_STRIKE  from 1 s on, Kurt mounted on the nearest live one of
#                               that model in his arena by the ported Rides code
#                               (Rides::mount): a walker at once (RideWalker, the C#'s
#                               --ride), the board and a bomber once rideable; in DANT_5
#                               an XE not rideable yet is called first, as the C#'s
#                               RideTest does (group 16 hit), and boards ~9 s later.
#   AIM=yaw,pitch               Kurt's yaw at spawn, and the scope's yaw and pitch the
#                               first time he snipes (degrees; pitch within +-50, down
#                               positive): a shot that does not depend on turns rounded
#                               through the trig table.
# The set pieces (FIXED=1: frame f of WALK is step f):
#   the bomber   LEVEL=7 AT=100,2320,-60,90 ARENA=DANT_5 RIDE=XE FIXED=1
#                WALK='fire@800-802;turnright@820-840;fire@840-842;forward@850-865;fire@866-868'
#   the XD2      LEVEL=7 AT=45,4868,-25,90 ARENA=DANT_9 RIDE=XD2 FIXED=1
#                WALK='forward@120-200;turnleft@200-230;forward@230-280'
#   the strike   LEVEL=5 AT=250,121,-1622,0 GIVE=SW_BONES AIM=0,1.25 FIXED=1
#                WALK='sniper@20-22;fire@80-82'
#   the holes    LEVEL=8 AT=20,3360,-360,0 ARENA=GUNT_10 AIM=123.80164,-26.158365 FIXED=1
#                WALK='sniper@150-152;fire@190-200'
ifneq ($(strip $(GIVE)),)
ifndef AUTOQUIT
$(error GIVE=$(GIVE) needs AUTOQUIT: the pickups at spawn are a harness-build feature)
endif
DEFINES += -DMDK_HW_GIVE='"$(GIVE)"'
endif
ifneq ($(strip $(RIDE)),)
ifndef AUTOQUIT
$(error RIDE=$(RIDE) needs AUTOQUIT: the ride at spawn is a harness-build feature)
endif
DEFINES += -DMDK_HW_RIDE='"$(RIDE)"'
endif
ifneq ($(strip $(AIM)),)
ifndef AUTOQUIT
$(error AIM=$(AIM) needs AUTOQUIT: the facing at spawn is a harness-build feature)
endif
DEFINES += -DMDK_HW_AIM='"$(AIM)"'
endif
# `make AUTOQUIT=90 EVENT=1` runs special_event 1 after one second of game time, the
# C#'s --event test option (Program.cs:53, Viewer.RunTests): 50 and below end the level
# (the arena breaks up, Kurt rises, the screen goes white). Since package 2 the flow
# then loads the next level (3 -> 4 -> 8 -> 5, the screens between skipped), which plays
# for real: the event is the first level's alone, as the C#'s test options are.
# `EVENT_EVERY=1` fires it one second into EVERY level, so a run chains level to level.
# Above 50 are the cutscenes (package 13), for the controller of Kurt's arena -- so start
# him in the arena whose script makes the call, with AT: LEVEL=7 AT=495,5005,24,0 EVENT=93
# (DANT_10's boss), LEVEL=8 AT=40,3221,-358,180 EVENT=91 (GUNT_10's), LEVEL=5
# AT=465,-6,-2263,0 with EVENT=61 (Gunter), 52 (the dog), 51 (the strike's cutscene half)
# or 81 (the end of the game: the run ends "game finished").
# Only main_3ds.cpp reads them: touch it after changing EVENT or EVENT_EVERY.
ifneq ($(strip $(EVENT)),)
ifndef AUTOQUIT
$(error EVENT=$(EVENT) needs AUTOQUIT: the test event is a harness-build feature)
endif
DEFINES += -DMDK_HW_EVENT=$(EVENT)
ifeq ($(strip $(EVENT_EVERY)),1)
DEFINES += -DMDK_HW_EVENT_EVERY=1
endif
else ifneq ($(strip $(EVENT_EVERY)),)
$(error EVENT_EVERY=$(EVENT_EVERY) needs EVENT=N: there is no event to repeat)
endif
# `make AUTOQUIT=90 DIE=1` hurts Kurt to death after one second of game time (the C#'s
# --die): the death writes sdmc:/3ds/mdk/saves/LASTGAME.sav and the main menu follows
# with "Continue" first, as the C#'s Handle(KurtDied) (package #19); add
# `MENU=Menu.Accept@2` to take it, which reloads the level. `SAVETEST=1` presses the pause menu's X (a full save) at 5 s
# and Y (its load) at 8 s, so the level reloads from the save. Only main_3ds.cpp reads
# them: touch it after changing DIE or SAVETEST.
# `make AUTOQUIT=90 SPLASH=0` boots into the main menu WITHOUT the INTRO1A splash
# (package #20c; the C#'s --menu), so MENU= presses reach the menu at once. Without
# it the splash shows first and the first press skips it, as in the C#. Only
# main_3ds.cpp reads it: touch it after changing SPLASH.
ifeq ($(strip $(SPLASH)),0)
ifeq ($(strip $(AUTOQUIT)),)
$(error SPLASH=0 needs AUTOQUIT: the splash switch is a harness-build feature)
endif
DEFINES += -DMDK_HW_NOSPLASH=1
endif
ifneq ($(strip $(DIE)),)
ifndef AUTOQUIT
$(error DIE=$(DIE) needs AUTOQUIT: the test death is a harness-build feature)
endif
DEFINES += -DMDK_HW_DIE=1
endif
ifneq ($(strip $(SAVETEST)),)
ifndef AUTOQUIT
$(error SAVETEST=$(SAVETEST) needs AUTOQUIT: the save test is a harness-build feature)
endif
DEFINES += -DMDK_HW_SAVETEST=1
endif
# `make AUTOQUIT=90 GOD=1 NOCLIP=1` starts the session with the pause menu's CHEATS page
# toggles on (gap package #21): god mode (no damage, no fall-out death) and noclip (the
# walking keys fly Kurt through everything, jump rises, the `down` action sinks). With a
# WALK= the noclip flight is visible without touching the console. Only main_3ds.cpp
# reads them: touch it after changing GOD or NOCLIP.
ifneq ($(strip $(GOD)),)
ifndef AUTOQUIT
$(error GOD=$(GOD) needs AUTOQUIT: the start-up cheat is a harness-build feature)
endif
DEFINES += -DMDK_HW_GOD=1
endif
ifneq ($(strip $(NOCLIP)),)
ifndef AUTOQUIT
$(error NOCLIP=$(NOCLIP) needs AUTOQUIT: the start-up cheat is a harness-build feature)
endif
DEFINES += -DMDK_HW_NOCLIP=1
endif
# `make AUTOQUIT=90 TOGGLES='enableGlowBillboards=0;shadowMapSize=128'` applies
# config.ini lines (';' between them, no spaces, no commas) over the console's own
# config, for the per-toggle cost profiles: one console config per run without
# writing the SD card. Only main_3ds.cpp reads it: touch it after changing TOGGLES.
# Kurt's three stores, for the A/B (the boot log's `sprites: kurtCompressed=...`
# line says which ran): TOGGLES='kurtCompressed=0;texture16=0' is the RGBA8
# expansion, TOGGLES='kurtCompressed=0' the RGBA5551 one, and no override (or
# TOGGLES='kurtCompressed=1') the ETC1A4 frames from romfs:/kurt (stage_romfs.sh;
# MDK_KURT_ETC1=0 builds a RomFS without them).
ifneq ($(strip $(TOGGLES)),)
ifndef AUTOQUIT
$(error TOGGLES=$(TOGGLES) needs AUTOQUIT: the config override is a harness-build feature)
endif
DEFINES += -DMDK_HW_TOGGLES='$(TOGGLES)'
endif
# `make AUTOQUIT=90 GSPASSALL=1` sends every whole-triangle unlit draw through the
# pass-through geometry shader (a harness aid): with the light off, what the
# geometry stage costs by itself. Only renderer_c3d.cpp reads it: touch it after.
ifneq ($(strip $(GSPASSALL)),)
ifndef AUTOQUIT
$(error GSPASSALL=$(GSPASSALL) needs AUTOQUIT: the probe is a harness-build feature)
endif
DEFINES += -DMDK_HW_GSPASSALL=1
endif
# `make AUTOQUIT=90 DRAWDEDUPE=0` turns the per-draw state dedupe off (a harness
# aid, for the owner's A/B profile): every per-draw combiner, buffer table,
# uniform and texture write is then sent on every draw, as before the dedupe
# (03_frame.inc, THE DEDUPE). DRAWDEDUPE=1, or unset, is the shipped dedupe.
# Only renderer_c3d.cpp reads it: touch it (or `make clean`) after changing it.
ifneq ($(strip $(DRAWDEDUPE)),)
ifndef AUTOQUIT
$(error DRAWDEDUPE=$(DRAWDEDUPE) needs AUTOQUIT: the A/B switch is a harness-build feature)
endif
ifeq ($(strip $(DRAWDEDUPE)),0)
DEFINES += -DMDK_HW_DRAWDEDUPE_OFF=1
else ifneq ($(strip $(DRAWDEDUPE)),1)
$(error DRAWDEDUPE=$(DRAWDEDUPE): use 0 (off, the A/B) or 1 (on, the default))
endif
endif
# `make AUTOQUIT=90 DSPAUDIO=0` keeps the software mix (a harness aid, for the
# owner's A/B profile): the shared mixer sums the voices on the ARM11 into one
# ndsp channel (audio_3ds.cpp), as before SPEC.md 8's DSP mixing. DSPAUDIO=1, or
# unset, is the shipped DSP path (audio_dsp_3ds.h: one ndsp channel per voice).
# main_3ds.cpp and audio_dsp_3ds.cpp read it: `make clean` after changing it.
ifneq ($(strip $(DSPAUDIO)),)
ifndef AUTOQUIT
$(error DSPAUDIO=$(DSPAUDIO) needs AUTOQUIT: the A/B switch is a harness-build feature)
endif
ifeq ($(strip $(DSPAUDIO)),0)
DEFINES += -DMDK_HW_DSPAUDIO_OFF=1
else ifneq ($(strip $(DSPAUDIO)),1)
$(error DSPAUDIO=$(DSPAUDIO): use 0 (software mix, the A/B) or 1 (DSP mixing, the default))
endif
endif
# `make AUTOQUIT=90 GLOWPROBE=1` adds one halo in front of the camera every
# frame (a harness aid): the glow billboards' path drawn on hardware without a
# script's explosion. Only main_3ds.cpp reads it: touch it after changing it.
ifneq ($(strip $(GLOWPROBE)),)
ifndef AUTOQUIT
$(error GLOWPROBE=$(GLOWPROBE) needs AUTOQUIT: the probe is a harness-build feature)
endif
DEFINES += -DMDK_HW_GLOWPROBE=1
endif
# `make SHADOWPROBE=n` (a harness aid, never a release): 8 pushes every receiver
# to the map's far end (all shadow, if the unit compares at all); 10 clears the
# map to the nearest depth and casts nothing (all shadow, and the caster pass's
# cost removed: the 2026-11-19 profile split). The receive pass's combiner probes
# went with it (the shadow is taken in the lit draw now). Only renderer_c3d.cpp
# reads it: touch it after changing SHADOWPROBE.
# `make TOGGLEWALK=1` (a harness aid, never a release): boot in the original
# look and switch the config's enhancements on one at a time, 120 frames apart,
# logging each before its first frame (main_3ds.cpp), so a console that hangs
# names the toggle in mdk.log's last line. `make clean` after changing it.
ifneq ($(TOGGLEWALK),)
DEFINES += -DMDK_TOGGLE_WALK=1
endif
ifneq ($(SHADOWPROBE),)
DEFINES += -DMDK_SHADOW_PROBE=$(SHADOWPROBE)
endif
# `make AUTOQUIT=90 FIXED=1` gives every frame exactly one 1/60 s step (the
# Linux --fixed-step), for frame-exact 3DS-vs-Linux comparisons with CAPTURE.
# Only main_3ds.cpp reads it: touch it after changing FIXED.
ifeq ($(strip $(FIXED)),1)
ifndef AUTOQUIT
$(error FIXED=1 needs AUTOQUIT: the fixed clock is a harness-build feature)
endif
DEFINES += -DMDK_HW_FIXED=1
endif
# `make AUTOQUIT=60 STREAM=1 LEVEL=<3..8>` starts the run in the stream after that
# level (the C#'s --stream=N; package #18b), its tube the default seed's; the flow
# then goes on to the next level. Only main_3ds.cpp reads it: touch it after
# changing STREAM.
ifeq ($(strip $(STREAM)),1)
ifndef AUTOQUIT
$(error STREAM=1 needs AUTOQUIT: the direct stream is a harness-build feature)
endif
DEFINES += -DMDK_HW_STREAM=1
endif
# `make AUTOQUIT=90 PROFILE=1` logs one `profile:` frame-period summary at exit
# (harness_3ds.h), what `hw_cycle.sh --profile` reads. Never combine with
# CAPTURE: the capture's SD write would be inside the measured frames.
ifeq ($(strip $(PROFILE)),1)
ifndef AUTOQUIT
$(error PROFILE=1 needs AUTOQUIT: the profile is a harness-build feature)
endif
ifneq ($(strip $(CAPTURE)),)
$(error PROFILE and CAPTURE together would measure the capture's SD stall)
endif
DEFINES += -DMDK_HW_PROFILE=1
endif

COMMONFLAGS := $(ARCH) -O2 -ffunction-sections -fdata-sections \
               -fno-math-errno -fno-strict-aliasing $(DEFINES)

INCLUDE := -I$(CORESRC) \
	-I$(SRCDIR) \
	-I$(BUILD) \
	-I$(DEVKITPRO)/libctru/include \
	-I$(PORTLIBS)/include

# -Werror=format is the shipped half of the printf contract (review-23 F1). AGENTS R10
# forbids -ffast-math and -Ofast because they CHANGE arithmetic; this flag changes no
# byte of codegen, it only refuses the build. `mdk::log3dsPrintf` carries
# __attribute__((format (printf, 1, 2))) and has 72 call sites in three files, and
# before this line the only automated reader of those arguments was a gate that
# adjudicates ONE refusal line -- a mismatch elsewhere printed a wrong figure into the
# file the test harness reads and built with a warning nobody had to read. Batch 117
# measured the tree clean for this flag (73 sources, 0 diagnostics) and measured its
# casts codegen-neutral; batch 118 makes the "it is a gate" claim TRUE by putting the
# flag here, because a diagnostic no gate fails is a notice (review-23 F1). Do NOT
# delete it to unblock a build: fix the call site instead.
# tests/scripts/arm_compile_check.sh keeps the mutants that prove the attribute itself
# has not been deleted, and it reads ARCH/DEFINES from this file but NOT this flag, on
# purpose: 26 of the 133 mutant sources are format defects BY DESIGN (review-23 F3).
CXXFLAGS := $(COMMONFLAGS) $(INCLUDE) -std=gnu++17 -fno-exceptions -fno-rtti \
	-Wall -Wextra -Wno-unused-parameter -Werror=format

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

# One DVLB, two entry points: DVLE[0] the vertex shader, DVLE[1] the lit
# program's geometry shader (Enhanced Mode lighting, mdk_c3d.g.pica).
SHADER_PICA  := $(CURDIR)/shaders/mdk_c3d.v.pica
SHADER_GPICA := $(CURDIR)/shaders/mdk_c3d.g.pica
# The lit program WITH the sun's shadow, one pass (Enhanced Mode sun shadows):
# the DVLB's third DVLE. The ORDER on the picasso line below is the DVLE order the
# backend indexes by (02_lifecycle.inc).
SHADER_SPICA := $(CURDIR)/shaders/mdk_litshadow.g.pica
# ... and the shadow caster pass's vertex shader: the fourth DVLE.
SHADER_CPICA := $(CURDIR)/shaders/mdk_caster.v.pica
# ... and the pass-through geometry shader: the fifth DVLE (mdk_pass.g.pica says
# why: the PICA must not leave geometry mode inside a command list).
SHADER_PPICA := $(CURDIR)/shaders/mdk_pass.g.pica
SHADER_SHBIN := $(BUILD)/mdk_c3d.shbin
SHADER_HDR   := $(BUILD)/mdk_c3d_shbin.h
SHADER_OBJ   := $(BUILD)/mdk_c3d_shbin.o

$(SHADER_SHBIN): $(SHADER_PICA) $(SHADER_GPICA) $(SHADER_SPICA) $(SHADER_CPICA) $(SHADER_PPICA) | $(BUILD)
	@echo "PICA $(notdir $(SHADER_PICA)) $(notdir $(SHADER_GPICA)) $(notdir $(SHADER_SPICA)) $(notdir $(SHADER_CPICA)) $(notdir $(SHADER_PPICA))"
	@$(PICASSO) -o $@ $(SHADER_PICA) $(SHADER_GPICA) $(SHADER_SPICA) $(SHADER_CPICA) $(SHADER_PPICA)

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
# installed. tools/build_cia.sh discovers it (or $MAKEROM), substitutes the
# staged RomFS path into cia/mdk.rsf, and exits non-zero when makerom is
# absent rather than producing a .cia that shows the wrong title.
$(TARGET).cia: $(TARGET).elf $(CIA_SMDH) $(CIA_ROMFS) $(CIA_DIR)/icon.png $(CIA_DIR)/banner.png $(CIA_DIR)/mdk.rsf tools/build_cia.sh
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
# in Debris::randomSpin each way). The default here is `fast`, not `off`
# (`arm-none-eabi-g++ -Q --help=common`, devkitARM 15.2.0; host gcc 13.3 too) --
# and NOT because -std=gnu++17 is a GNU dialect, which an earlier draft of this
# comment claimed: the dialect is not the cause, because -std=c++17, -std=gnu++17
# and -std=c++20 all report `fast` on BOTH compilers (review-29 F2, re-measured).
# Do not infer "ISO mode means off" from this file; there is no dialect that makes
# the flag bite on this target.
#
# The reason the flag has nothing to gate is that VMLA.F32 is NOT a fused MAC:
# VMLA/VMLS/VNMLA/VNMLS round their product to the destination format before
# accumulating, and the single-rounded forms are the VFPv4 VFMA/VFMS this target
# cannot encode. Forming a VMLA is therefore value-preserving -- for normal-range
# intermediates, which is all this path reaches: the target runs with FPSCR.FZ set
# (AGENTS.md R1, mdk_3ds/src/platform/vfp_fpscr.h) and the host does not, so a
# DENORMAL product would flush on the 3DS and not here. randomSpin's half-angles
# are (draw - 0x41c2)*degrees/2, so its triple products are ~4e-16 at falloff f=1
# and only reach the 1.18e-38 flush bound if f < ~3e-8, i.e. if the shatter point
# sits essentially exactly on the box's far corner: not reachable in the shipped
# geometry, and not proven impossible for arbitrary geometry. A future caller with
# subnormal-capable magnitudes would not inherit that. Which is why the flag does
# not mention it. Measured four
# ways on this tree with the shipped flags (docs/PROGRESS_LOG.md, "Iteration 87"):
#   * `float t = a*b; return c + t;` -- twice-rounded by construction -- compiles
#     to ONE vmla.f32 under -ffp-contract=off. A fused VMLA could not do that.
#   * the same source at -mfpu=vfpv4 gives vmla.f32 under =off and vfma.f32 under
#     =fast: the flag gates the FUSED form, and only the fused form.
#   * __builtin_fmaf at -mfpu=vfp is `b fmaf`, a libm call, because VFPv2 has no
#     fused instruction to use (the fact docs/OPEN_ITEMS.md records for
#     frame_blend.cpp).
#   * the shipped randomSpin disassembly's own operand grouping, replayed with a
#     rounded product, reproduces 0/72 of the oracle's pinned spin components
#     wrong; replayed as a fused MAC it gets 23/72 wrong on 12 of 18 pieces. That
#     last pair is a scratch model of the disassembly, not a committed harness, and
#     review-29 F7 is right that it cannot be re-derived from this tree today.
# So the shipped object's MACs compute the same twice-rounded value as the C
# source and as the host harness -- for the flags the two host build systems
# actually use: neither tests/scripts/Makefile nor mdk_linux/CMakeLists.txt sets
# -march, so the x86-64 baseline has no fused multiply-add to contract into
# (measured: the same source at -march=native on this FMA-capable host gives 4
# vfmadd*, at the default 0). A host build with an FMA-capable -march WOULD
# contract and would stop describing this object; pinning -ffp-contract=off in the
# host builds is the named follow-up in docs/OPEN_ITEMS.md. There is no VMLA
# rounding gap to measure and no expression to "fix". What IS still unmeasured on
# Old-3DS hardware is the libm
# leaf -- the shipped sinf/cosf come from newlib, and only glibc's have been
# checked bit-for-bit against MathF.Sin/MathF.Cos. Look there first if a capture
# ever shows a divergence.

clean:
	@echo "CLEAN"
	@rm -rf $(BUILD) $(TARGET).elf $(TARGET).3dsx $(TARGET).cia $(TARGET).map
