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

test.shbin: pass.g.pica ../../shaders/mdk_c3d.v.pica ../../shaders/mdk_c3d.g.pica ../../shaders/mdk_shadow.g.pica ../../shaders/mdk_caster.v.pica
	$(DEVKITPRO)/tools/bin/picasso -o $@ ../../shaders/mdk_c3d.v.pica ../../shaders/mdk_c3d.g.pica ../../shaders/mdk_shadow.g.pica ../../shaders/mdk_caster.v.pica pass.g.pica

shbin.o: shbin.s test.shbin
	$(PREFIX)gcc $(ARCH) -c $< -o $@

FORCE:

clean:
	rm -f *.o gstest.elf gstest.3dsx test.shbin
