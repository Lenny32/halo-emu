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

LDS := vendor/lds/rom_symbols_ble_v1_2.lds vendor/lds/rom_symbols_lc3_v1_2.lds

CFLAGS := -mcpu=cortex-m55 -mthumb -Os -g -ffreestanding -fno-builtin \
	-Wall -Wextra -Wno-unused-parameter \
	-ffunction-sections -fdata-sections \
	-Ishim -Isrc \
	-Ivendor/include \
	-Ivendor/include/common \
	-Ivendor/include/ip/hl \
	-Ivendor/include/ip/ahi \
	-Ivendor/include/ipc \
	-Ivendor/include/isooshm \
	-Ivendor/include/ke \
	-Ivendor/include/plf/ble \
	-Ivendor/include/rwip

LDFLAGS := -nostdlib -nostartfiles -Wl,--gc-sections -Wl,-T,rom-stub.ld \
	-Wl,--no-warn-rwx-segments

all: $(BUILD)/rom-stub-v1_2.bin $(BUILD)/rom-stub-v1_2.syms

# Bare-metal test firmware (gate evidence: no zephyr.bin exists on this
# host).  Linked against the same pinned ROM addresses via the vendored
# symbol maps — exactly how the real firmware links against the ROM.
test-fw: $(BUILD)/fw_blesmoke.bin

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

$(BUILD)/%.o: src/%.c src/stub.h src/halo_rom_ipc.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# The set of implemented pinned symbols is derived from the objects: every
# global hstub_<name> gets a veneer at <name>'s pinned address.
$(BUILD)/implemented.txt: $(OBJS)
	$(NM) $(OBJS) | sed -n 's/^.* [TW] hstub_\(.*\)$$/\1/p' | sort -u > $@

$(BUILD)/veneers.S $(BUILD)/rom-stub-v1_2.syms &: $(BUILD)/implemented.txt $(LDS) gen_rom_layout.py
	python3 gen_rom_layout.py --lds $(LDS) --impl $(BUILD)/implemented.txt \
		--veneers $(BUILD)/veneers.S --syms $(BUILD)/rom-stub-v1_2.syms

$(BUILD)/veneers.o: $(BUILD)/veneers.S
	$(CC) -mcpu=cortex-m55 -mthumb -c -o $@ $<

$(BUILD)/rom-stub-v1_2.elf: $(OBJS) $(BUILD)/veneers.o rom-stub.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(BUILD)/veneers.o

$(BUILD)/rom-stub-v1_2.bin: $(BUILD)/rom-stub-v1_2.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)

.PHONY: all clean
