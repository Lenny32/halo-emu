# Makefile — synthetic BLE/LC3 ROM stub (ROM symbol map v1_2).
#
# Toolchain: arm-none-eabi-gcc.  init.sh installs a local copy under
# deps/toolchain if the system has none; override with CROSS=...
#
# Outputs (build/):
#   rom-stub-v1_2.bin   loaded by the halo machine at 0x00090000
#   rom-stub-v1_2.syms  "<idx> <addr> <name>" — trap decoding for QEMU
#   rom-stub-v1_2.elf   for debugging (gdb add-symbol-file)

CROSS ?= $(firstword $(wildcard ../deps/toolchain/bin/arm-none-eabi-gcc) \
                     $(shell command -v arm-none-eabi-gcc 2>/dev/null))
ifeq ($(CROSS),)
$(error arm-none-eabi-gcc not found - run ../init.sh or set CROSS=)
endif
CC := $(CROSS)
PREFIX := $(patsubst %gcc,%,$(CROSS))
OBJCOPY := $(PREFIX)objcopy
NM := $(PREFIX)nm

BUILD := build
SRCS := $(wildcard src/*.c)
OBJS := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))

# LC3 codec: Google's liblc3, fetched by ../init.sh into ../deps/liblc3.
# The firmware exchanges real LC3 bitstreams with the phone, so the ROM's
# lc3_api_* entry points need a conformant codec, not a passthrough
# (rom-stub/src/stub_lc3.c is the ABI glue).
LC3_SRC := ../deps/liblc3
LC3_SRCS := $(wildcard $(LC3_SRC)/src/*.c)
LC3_OBJS := $(patsubst $(LC3_SRC)/src/%.c,$(BUILD)/lc3/%.o,$(LC3_SRCS))
LC3_LIB := $(BUILD)/liblc3.a

LDS := vendor/lds/rom_symbols_ble_v1_2.lds vendor/lds/rom_symbols_lc3_v1_2.lds

# -mfloat-abi=hard: liblc3 is floating point, and hard- and soft-float
# objects cannot be linked together, so the whole stub uses the FPU the
# firmware has already enabled in SystemInit (it is hard-float itself).
ARCHFLAGS := -mcpu=cortex-m55 -mthumb -mfloat-abi=hard

CFLAGS := $(ARCHFLAGS) -Os -g -ffreestanding -fno-builtin \
	-Wall -Wextra -Wno-unused-parameter \
	-ffunction-sections -fdata-sections \
	-Ishim -Isrc \
	-I$(LC3_SRC)/include \
	-Ivendor/include \
	-Ivendor/include/common \
	-Ivendor/include/ip/hl \
	-Ivendor/include/ip/ahi \
	-Ivendor/include/ipc \
	-Ivendor/include/isooshm \
	-Ivendor/include/ke \
	-Ivendor/include/plf/ble \
	-Ivendor/include/rwip

# liblc3 is third-party: build it warning-clean-by-omission and let GCC
# use its float builtins (that turns fabsf/sqrtf/floorf/... into single
# FPU instructions, so no libm is needed — only libgcc's 64-bit divide
# is left over).
LC3_CFLAGS := $(ARCHFLAGS) -Os -g -fno-math-errno \
	-ffunction-sections -fdata-sections \
	-I$(LC3_SRC)/include -I$(LC3_SRC)/src

LDFLAGS := -nostdlib -nostartfiles -Wl,--gc-sections -Wl,-T,rom-stub.ld \
	-Wl,--no-warn-rwx-segments
LDLIBS := -lgcc

all: $(BUILD)/rom-stub-v1_2.bin $(BUILD)/rom-stub-v1_2.syms

# Bare-metal test firmware (gate evidence: no zephyr.bin exists on this
# host).  fw_blesmoke links against the same pinned ROM addresses via the
# vendored symbol maps — exactly how the real firmware links against the
# ROM; fw_dispsmoke (ticket 0029) exercises the display path and needs no
# ROM symbols.
test-fw: $(BUILD)/fw_blesmoke.bin $(BUILD)/fw_dispsmoke.bin

$(BUILD)/fw_blesmoke.elf: test/fw_blesmoke.c test/fw_start.c test/fw.ld $(LDS) | $(BUILD)
	$(CC) $(CFLAGS) -nostdlib -nostartfiles \
		-Wl,-T,test/fw.ld $(addprefix -Wl$(comma)-T$(comma),$(LDS)) \
		-Wl,--no-warn-rwx-segments \
		-o $@ test/fw_blesmoke.c test/fw_start.c

comma := ,

$(BUILD)/fw_blesmoke.bin: $(BUILD)/fw_blesmoke.elf
	$(OBJCOPY) -O binary $< $(BUILD)/fw_blesmoke.tmp.bin
	{ head -c 2048 /dev/zero; cat $(BUILD)/fw_blesmoke.tmp.bin; } > $@
	rm -f $(BUILD)/fw_blesmoke.tmp.bin

$(BUILD)/fw_dispsmoke.elf: test/fw_dispsmoke.c test/fw.ld | $(BUILD)
	$(CC) $(CFLAGS) -nostdlib -nostartfiles \
		-Wl,-T,test/fw.ld \
		-Wl,--no-warn-rwx-segments \
		-o $@ test/fw_dispsmoke.c

$(BUILD)/fw_dispsmoke.bin: $(BUILD)/fw_dispsmoke.elf
	$(OBJCOPY) -O binary $< $(BUILD)/fw_dispsmoke.tmp.bin
	{ head -c 2048 /dev/zero; cat $(BUILD)/fw_dispsmoke.tmp.bin; } > $@
	rm -f $(BUILD)/fw_dispsmoke.tmp.bin

$(BUILD)/%.o: src/%.c src/stub.h src/halo_rom_ipc.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(LC3_SRC)/include/lc3.h:
	@echo "rom-stub: liblc3 missing at $(LC3_SRC) — run ../init.sh" >&2; exit 1

$(BUILD)/lc3/%.o: $(LC3_SRC)/src/%.c $(LC3_SRC)/include/lc3.h | $(BUILD)/lc3
	$(CC) $(LC3_CFLAGS) -c -o $@ $<

# An archive, so rom-stub.ld can place the codec's text and its ~80 KB of
# tables in their own region without pattern-matching object paths.
$(LC3_LIB): $(LC3_OBJS)
	rm -f $@
	$(PREFIX)ar rcs $@ $(LC3_OBJS)

$(BUILD)/lc3:
	mkdir -p $@

# The set of implemented pinned symbols is derived from the objects: every
# global hstub_<name> gets a veneer at <name>'s pinned address.
$(BUILD)/implemented.txt: $(OBJS)
	$(NM) $(OBJS) | sed -n 's/^.* [TW] hstub_\(.*\)$$/\1/p' | sort -u > $@

$(BUILD)/veneers.S $(BUILD)/rom-stub-v1_2.syms &: $(BUILD)/implemented.txt $(LDS) gen_rom_layout.py
	python3 gen_rom_layout.py --lds $(LDS) --impl $(BUILD)/implemented.txt \
		--veneers $(BUILD)/veneers.S --syms $(BUILD)/rom-stub-v1_2.syms

$(BUILD)/veneers.o: $(BUILD)/veneers.S
	$(CC) $(ARCHFLAGS) -c -o $@ $<

$(BUILD)/rom-stub-v1_2.elf: $(OBJS) $(BUILD)/veneers.o $(LC3_LIB) rom-stub.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(BUILD)/veneers.o \
		$(LC3_LIB) $(LDLIBS)

$(BUILD)/rom-stub-v1_2.bin: $(BUILD)/rom-stub-v1_2.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

.PHONY: all clean
