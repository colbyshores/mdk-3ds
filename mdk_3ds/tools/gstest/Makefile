# gstest -- see main.c. A hardware probe, not part of the game build.
DEVKITPRO ?= /opt/devkitpro
DEVKITARM ?= $(DEVKITPRO)/devkitARM
PREFIX := $(DEVKITARM)/bin/arm-none-eabi-
ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
STAGES ?= 1,2,3
CFLAGS := -g -O2 -Wall $(ARCH) -D__3DS__ -I$(DEVKITPRO)/libctru/include -DGSTEST_STAGES="$(STAGES)"
LDFLAGS := -specs=3dsx.specs -g $(ARCH)
LIBS := -L$(DEVKITPRO)/libctru/lib -lcitro3d -lctru -lm

gstest.3dsx: gstest.elf
	$(DEVKITPRO)/tools/bin/3dsxtool $< $@

gstest.elf: main.o shbin.o
	$(PREFIX)gcc $(LDFLAGS) -o $@ $^ $(LIBS)

main.o: main.c FORCE
	$(PREFIX)gcc $(CFLAGS) -c $< -o $@

# The game's own DVLB, in the game's DVLE order (mdk_3ds/Makefile): 0 the vertex
# shader, 1 the lit GS, 2 the shadowed lit GS, 3 the caster VS, 4 the
# pass-through GS, 5 the baked light's VS (lightmap uv), 6 its pass-through GS.
GAME_SHADERS := ../../shaders/mdk_c3d.v.pica ../../shaders/mdk_c3d.g.pica \
                ../../shaders/mdk_litshadow.g.pica ../../shaders/mdk_caster.v.pica \
                ../../shaders/mdk_pass.g.pica ../../shaders/mdk_lightmap.v.pica \
                ../../shaders/mdk_passlm.g.pica
test.shbin: $(GAME_SHADERS)
	$(DEVKITPRO)/tools/bin/picasso -o $@ $(GAME_SHADERS)

shbin.o: shbin.s test.shbin
	$(PREFIX)gcc $(ARCH) -c $< -o $@

FORCE:

clean:
	rm -f *.o gstest.elf gstest.3dsx test.shbin
