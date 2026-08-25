/*
 * Halo machine — Alif Balletto B1 (Cortex-M55 RTSS-HE) as used by the
 * halo firmware.
 *
 * Models just enough of the SoC to execute the unmodified firmware
 * application image (zephyr.bin / zephyr.signed.bin loaded at MRAM
 * 0x80020000, vector table at 0x80020800):
 *
 *  - Cortex-M55 with the M-profile security extension (the app runs
 *    Secure and programs one SAU NS region itself in SystemInit).
 *  - Memory map: MRAM 2 MiB @ 0x80000000 — plain RAM by default, or
 *    mmap-backed by a host file (machine option `mram-file=...`,
 *    MAP_SHARED) so littlefs/settings survive restarts like the real
 *    non-volatile MRAM; ITCM 512 KiB @ 0 (alias
 *    0x58000000), DTCM 1.5 MiB @ 0x20000000 (alias 0x58800000, the
 *    "global" address the SE mailbox and CDC200 use), and a RAM window
 *    at 0x00090000..0x00160000 reserved for the synthetic BLE/LC3 ROM
 *    stub (alias 0x58090000).
 *  - ITGU/DTGU (TCM gating units, in the PPB next to the NVIC):
 *    SystemInit's sau_tcm_ns_setup() reads CFG.BLKSZ and loops over the
 *    LUT registers; they are modeled as readable/writable state with no
 *    gating effect.
 *  - Console UART3: DesignWare 16550 @ 0x4901B000, reg-shift 2, IRQ 127.
 *    QEMU's serial-mm covers the 16550 register file; the DesignWare
 *    extras (DLF @ 0xC0 etc.) fall through to the background
 *    unimplemented-device region (read 0 / ignore writes).
 *  - SE fake: the MHUv2 mailbox pair (sender 0x40050000 / IRQ 38,
 *    receiver 0x40040000 / IRQ 37) with an ack-and-zero Secure Enclave
 *    responder behind it (halo_se.c).
 *  - Boot-critical peripherals (see the halo_*.c models): CMSDK
 *    watchdog stub (never expires on its own; wdt-fire injects one
 *    timeout), DW APB RTC (the tickless-idle counter), Alif ADC12 with
 *    a runtime-settable battery voltage, UTIMER3 as a PWM sink for the
 *    LED (duty readable), eleven DW GPIO banks, and two DW I2C
 *    controllers backed by QEMU I2C buses (absent addresses NACK
 *    cleanly).  Runtime controls (button, charger, battery, LED, wdt)
 *    are QOM properties on /machine — see the block above
 *    halo_button_drive() — driven by halo-emu's control socket.
 *  - Display: CDC200 controller @ 0x49031000 as a QEMU graphic console
 *    (halo_cdc200.c), a DSI-host happy-path fake @ 0x49032000
 *    (halo_dsi.c), and ack-everything I2C1 register-file targets for
 *    the vga020 panel (0x54) and TPS65132 PMIC (0x3E).
 *  - Audio: I2S0 @ 0x49014000 / IRQ 141 as the speaker (halo_i2s.c),
 *    sinking to a WAV file and/or the host audio backend; LPPDM
 *    @ 0x43002000 / IRQ 49 as the microphone (halo_pdm.c), drained by
 *    QEMU's PL330 at 0x400C0000 (dma2) because the firmware's PDM
 *    driver has no non-DMA fallback.
 *  - Everything else (CGU/AON/VBAT scratch, EVTRTR, ...) is
 *    background-mapped as unimplemented-device so raw boot-time pokes
 *    trace with `-d unimp` instead of faulting.  With BLE absent the
 *    firmware is expected to park in alif_ble_enable() until the
 *    synthetic ROM ticket.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/arm/armv7m.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-clock.h"
#include "hw/char/serial-mm.h"
#include "hw/core/irq.h"
#include "hw/i2c/i2c.h"
#include "hw/misc/unimp.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "system/reset.h"
#include "qom/object.h"
#include "ui/input.h"
#include "halo.h"

/* Main CPU / SysTick clock: CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC */
#define HALO_SYSCLK_FRQ 160000000
/* SysTick external reference clock (LFXO); Zephyr uses CLKSOURCE=CPU */
#define HALO_REFCLK_FRQ 32768

#define HALO_NUM_IRQ 480 /* CONFIG_NUM_IRQS: external NVIC lines */

/* Memory map (zephyr.map "Memory Configuration" + Alif global aliases) */
#define HALO_MRAM_BASE 0x80000000
#define HALO_MRAM_SIZE (2 * MiB)
#define HALO_ITCM_BASE 0x00000000
#define HALO_ITCM_SIZE (512 * KiB)
#define HALO_ITCM_ALIAS 0x58000000
#define HALO_ROMWIN_BASE 0x00090000 /* synthetic BLE/LC3 ROM window */
/*
 * ..0x00190000.  The pinned ROM symbols only reach 0x00141000; the rest
 * is the stub's own text/data, the doorbell rings at 0x00156000 /
 * 0x0015A000, and — since ticket 0032 — liblc3's ~115 KB of code and
 * tables above 0x00160000 (rom-stub/rom-stub.ld).
 */
#define HALO_ROMWIN_SIZE 0x00100000
#define HALO_ROMWIN_ALIAS 0x58090000
#define HALO_DTCM_BASE 0x20000000
#define HALO_DTCM_SIZE 0x00180000 /* 1.5 MiB incl. .alif_ns @ +0xE0000 */
#define HALO_DTCM_ALIAS 0x58800000 /* LOCAL_TO_GLOBAL() base */

/* Application image: MRAM slot0 @ +0x20000, CONFIG_ROM_START_OFFSET pad */
#define HALO_APP_BASE 0x80020000
#define HALO_APP_MAX_SIZE (HALO_MRAM_SIZE - 0x20000)
#define HALO_APP_VTOR 0x80020800

/* Console UART3 (DW 16550, ns16550 in the devicetree) */
#define HALO_UART3_BASE 0x4901B000
#define HALO_UART3_IRQ 127
#define HALO_UART3_CLK 40000000 /* refclk 40 MHz -> baudbase = clk/16 */

/* SE mailbox: MHUv2 receiver/sender pair (halo_se.c) */
#define HALO_MHU_RECV_BASE 0x40040000
#define HALO_MHU_RECV_IRQ 37
#define HALO_MHU_SEND_BASE 0x40050000
#define HALO_MHU_SEND_IRQ 38

#define HALO_WDOG_BASE 0x40100000

#define HALO_RTC_BASE 0x42000000
#define HALO_RTC_IRQ 58

/* ADC0 windows (alif,adc: adc/analog, comparator, AON) + DONE1 IRQ */
#define HALO_ADC_BASE 0x49020000
#define HALO_ADC_COMP_BASE 0x49023000
#define HALO_ADC_AON_BASE 0x1A604000
#define HALO_ADC_DONE1_IRQ 154

/* UTIMER3 (LED PWM) + shared utimer global block */
#define HALO_UTIMER3_BASE 0x48004000
#define HALO_UTIMER_GLB_BASE 0x48000000

#define HALO_I2C0_BASE 0x49010000
#define HALO_I2C0_IRQ 132
#define HALO_I2C1_BASE 0x49011000
#define HALO_I2C1_IRQ 133

/* Audio (ticket 0032): I2S0 speaker path — interrupt/FIFO driven, the
 * board's i2s0 node has no `dmas` (halo_i2s.c) */
#define HALO_I2S0_BASE 0x49014000
#define HALO_I2S0_IRQ 141

/*
 * Microphone: LPPDM (halo_pdm.c) drained by dma2, the SoC's PL330.
 * The board wires 5 channels whose completion IRQs are NVIC 0..4, with
 * the abort line on 32 (boards/arm/halo/halo.dts).  QEMU's PL330
 * exposes the abort as sysbus IRQ 0 and event N as sysbus IRQ 1+N.
 */
#define HALO_PDM_BASE 0x43002000
#define HALO_PDM_IRQ 49
#define HALO_DMA2_BASE 0x400C0000
#define HALO_DMA2_CHANNELS 5
#define HALO_DMA2_ABORT_IRQ 32
#define HALO_DMA2_NUM_EVENTS 16
/* LPPDM_DMA_REQ: the request line the event router maps the LPPDM
 * watermark onto (dmas = <&dma2 4 30>) */
#define HALO_PDM_DMA_REQ 30

/* CDC200 display controller + DSI host fake (halo_cdc200.c, halo_dsi.c) */
#define HALO_CDC200_BASE 0x49031000
#define HALO_CDC200_SCANLINE_IRQ 333
#define HALO_DSI_BASE 0x49032000
#define HALO_DSI_IRQ 343

/* I2C1 display-path targets (halo_i2c_regfile.c) */
#define HALO_VGA020_I2C_ADDR 0x54  /* panel: 16-bit register address */
#define HALO_TPS65132_I2C_ADDR 0x3E /* display PMIC: 8-bit registers */

/* BLE doorbell device (synthetic ROM stub transport, halo_ble.c):
 * doorbell page + fake UTIMER0 channel; IRQ = UTIMER0 capture A, the line
 * the firmware's BLE sync-timer driver connects (sync_timer.c). */
#define HALO_BLE_DOORBELL_BASE 0x4904E000
#define HALO_BLE_UTIMER0_BASE 0x48001000
#define HALO_BLE_IRQ 377

/*
 * TCM gating units (Alif "TGU", in the PPB): base +0x0 CTRL, +0x4 CFG
 * (BLKSZ in [3:0], block size = 2^(BLKSZ+5)), LUTs from +0x10.
 * BLKSZ=7 (4 KiB blocks) keeps the largest TCM (1.5 MiB DTCM) at 384
 * blocks = 12 LUT registers.
 */
#define HALO_ITGU_BASE 0xE001E500
#define HALO_DTGU_BASE 0xE001E600
#define HALO_TGU_REGION_SIZE 0x100
#define HALO_TGU_BLKSZ 0x7
#define HALO_TGU_NUM_LUT ((HALO_TGU_REGION_SIZE - 0x10) / 4)

typedef struct HaloTGU {
    MemoryRegion iomem;
    uint32_t ctrl;
    uint32_t lut[HALO_TGU_NUM_LUT];
} HaloTGU;

struct HaloMachineState {
    MachineState parent;

    ARMv7MState armv7m;
    MemoryRegion mram;
    MemoryRegion itcm;
    MemoryRegion itcm_alias;
    MemoryRegion romwin;
    MemoryRegion romwin_alias;
    MemoryRegion dtcm;
    MemoryRegion dtcm_alias;
    MemoryRegion expslv;
    MemoryRegion expmst;
    MemoryRegion m55he_cfg;
    HaloTGU itgu;
    HaloTGU dtgu;
    Clock *sysclk;
    Clock *refclk;

    char *mram_file;
    char *rom_file;
    char *ble_symfile;
    char *i2s_wav_out;
    char *pdm_wav_in;
    char *audiodev;
    uint32_t init_svtor;

    /* Runtime-controls plumbing (ticket 0031) */
    DeviceState *lpgpio_dev;  /* button on pin 1, active-low */
    DeviceState *gpio0_dev;   /* charge-control output on pin 6 */
    DeviceState *gpio1_dev;   /* charger-state input on pin 3 */
    DeviceState *gpio8_dev;   /* speaker SD_MODE output on pin 5 */
    DeviceState *adc_dev;
    DeviceState *utimer_dev;
    DeviceState *wdog_dev;
    DeviceState *i2s_dev;     /* speaker (ticket 0032) */
    DeviceState *pdm_dev;     /* microphone (ticket 0032) */
    bool button_pressed;
    bool charger_connected;
    bool sram_preserve;       /* skip the TCM zeroing on the next reset */
};

#define TYPE_HALO_MACHINE MACHINE_TYPE_NAME("halo")
OBJECT_DECLARE_SIMPLE_TYPE(HaloMachineState, HALO_MACHINE)

static uint64_t halo_tgu_read(void *opaque, hwaddr offset, unsigned size)
{
    HaloTGU *tgu = opaque;

    switch (offset) {
    case 0x0: /* CTRL */
        return tgu->ctrl;
    case 0x4: /* CFG */
        return HALO_TGU_BLKSZ;
    default:
        if (offset >= 0x10) {
            return tgu->lut[(offset - 0x10) / 4];
        }
        return 0;
    }
}

static void halo_tgu_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    HaloTGU *tgu = opaque;

    switch (offset) {
    case 0x0: /* CTRL */
        tgu->ctrl = value;
        break;
    case 0x4: /* CFG is RO */
        break;
    default:
        if (offset >= 0x10) {
            tgu->lut[(offset - 0x10) / 4] = value;
        }
        break;
    }
}

static const MemoryRegionOps halo_tgu_ops = {
    .read = halo_tgu_read,
    .write = halo_tgu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void halo_make_ram(MemoryRegion *mr, const char *name,
                          hwaddr base, hwaddr size)
{
    memory_region_init_ram(mr, NULL, name, size, &error_fatal);
    memory_region_add_subregion(get_system_memory(), base, mr);
}

/* Singleton back-pointer for the cross-model hook below (max_cpus = 1,
 * one machine per process). */
static HaloMachineState *halo_machine;

/* halo.h: a CMSDK-watchdog reset (halo_wdog.c) is warm — the TCMs and
 * with them the firmware's watchdog-fired __noinit magic survive. */
void halo_sram_preserve_next_reset(void)
{
    if (halo_machine) {
        halo_machine->sram_preserve = true;
    }
}

/*
 * The SE's SoC reset (sys_reboot on the Balletto is an SE service, see
 * halo_se.c) power-cycles the TCMs: on hardware __noinit data does not
 * survive a reboot.  Model that by zeroing ITCM and DTCM on every system
 * reset — otherwise stale noinit magics (e.g. ble_lua's) make the
 * firmware skip GATT re-registration against the freshly reset ROM stub
 * and BLE comes back dead.  MRAM (non-volatile) and the ROM window (the
 * loaded stub image) keep their contents.  A watchdog reset skips the
 * zeroing once (sram_preserve, set by halo_wdog.c).  The GPIO banks
 * keep their external input levels across resets themselves (a held
 * button stays held through a reboot, see halo_gpio.c).
 */
static void halo_sram_reset(void *opaque)
{
    HaloMachineState *hms = opaque;

    if (hms->sram_preserve) {
        hms->sram_preserve = false;
        return;
    }
    memset(memory_region_get_ram_ptr(&hms->itcm), 0, HALO_ITCM_SIZE);
    memset(memory_region_get_ram_ptr(&hms->dtcm), 0, HALO_DTCM_SIZE);
}

static void halo_make_alias(MemoryRegion *mr, const char *name,
                            MemoryRegion *orig, hwaddr base)
{
    memory_region_init_alias(mr, NULL, name, orig, 0,
                             memory_region_size(orig));
    memory_region_add_subregion(get_system_memory(), base, mr);
}

/*
 * DW GPIO banks (halo_gpio.c).  Every pin has its own NVIC line,
 * consecutive from irq_base; the Zephyr ISR resolves the pin from
 * INTSTATUS, not the vector, so a straight 1:1 wiring suffices.
 */
static const struct {
    const char *name;
    hwaddr base;
    int irq_base;
    uint32_t ngpios;
    uint32_t in_default;
} halo_gpio_banks[] = {
    { "gpio0", 0x49000000, 179, 8, 0 },
    { "gpio1", 0x49001000, 187, 8, 0 },
    { "gpio2", 0x49002000, 195, 8, 0 },
    { "gpio3", 0x49003000, 203, 8, 0 },
    { "gpio4", 0x49004000, 211, 8, 0 },
    { "gpio5", 0x49005000, 219, 8, 0 },
    { "gpio6", 0x49006000, 227, 8, 0 },
    { "gpio7", 0x49007000, 235, 8, 0 },
    { "gpio8", 0x49008000, 243, 8, 0 },
    { "gpio9", 0x49009000, 251, 3, 0 },
    /* LPGPIO pin 1 is the button: active low with an external pull-up,
     * so it idles high (released). */
    { "lpgpio", 0x42002000, 171, 2, 1u << 1 },
};

static void halo_create_peripherals(HaloMachineState *hms,
                                    DeviceState *armv7m)
{
    SysBusDevice *sbd;
    DeviceState *dev;

    /* MHUv2 pair + Secure Enclave responder */
    dev = qdev_new("halo-se");
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, HALO_MHU_RECV_BASE);
    sysbus_mmio_map(sbd, 1, HALO_MHU_SEND_BASE);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(armv7m, HALO_MHU_RECV_IRQ));
    sysbus_connect_irq(sbd, 1, qdev_get_gpio_in(armv7m, HALO_MHU_SEND_IRQ));

    /* watchdog stub: armed and fed by the firmware; never expires on
     * its own, but the control socket's wdt-fire verb injects one full
     * timeout (NMI, then the RESEN warm reset — see halo_wdog.c) */
    dev = qdev_new("halo-wdog");
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, HALO_WDOG_BASE);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in_named(armv7m, "NMI", 0));
    hms->wdog_dev = dev;

    /* DW APB RTC: the cortex-m idle timer */
    dev = qdev_new("halo-rtc");
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, HALO_RTC_BASE);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(armv7m, HALO_RTC_IRQ));

    /* ADC0: VBAT on channel 4 (default battery-raw reads as 3.9 V) */
    dev = qdev_new("halo-adc");
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, HALO_ADC_BASE);
    sysbus_mmio_map(sbd, 1, HALO_ADC_COMP_BASE);
    sysbus_mmio_map(sbd, 2, HALO_ADC_AON_BASE);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(armv7m, HALO_ADC_DONE1_IRQ));
    hms->adc_dev = dev;

    /* UTIMER3 PWM sink (LED duty cycle recorder) */
    dev = qdev_new("halo-utimer");
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, HALO_UTIMER3_BASE);
    sysbus_mmio_map(sbd, 1, HALO_UTIMER_GLB_BASE);
    hms->utimer_dev = dev;

    for (unsigned i = 0; i < ARRAY_SIZE(halo_gpio_banks); i++) {
        dev = qdev_new("halo-dwgpio");
        qdev_prop_set_uint32(dev, "ngpios", halo_gpio_banks[i].ngpios);
        qdev_prop_set_uint32(dev, "in-default",
                             halo_gpio_banks[i].in_default);
        sbd = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, halo_gpio_banks[i].base);
        for (unsigned pin = 0; pin < halo_gpio_banks[i].ngpios; pin++) {
            sysbus_connect_irq(sbd, pin,
                qdev_get_gpio_in(armv7m, halo_gpio_banks[i].irq_base + pin));
        }
        if (!strcmp(halo_gpio_banks[i].name, "gpio0")) {
            hms->gpio0_dev = dev;
        } else if (!strcmp(halo_gpio_banks[i].name, "gpio1")) {
            hms->gpio1_dev = dev;
        } else if (!strcmp(halo_gpio_banks[i].name, "gpio8")) {
            hms->gpio8_dev = dev;
        } else if (!strcmp(halo_gpio_banks[i].name, "lpgpio")) {
            hms->lpgpio_dev = dev;
        }
    }

    /*
     * DW I2C controllers — QEMU's upstream designware-i2c model.
     * Targets (IMU, camera, panel — later tickets) attach to the
     * "i2c-bus" child bus; absent addresses NACK the start, which the
     * Zephyr driver turns into a clean -EIO via TX_ABRT.
     */
    dev = qdev_new("designware-i2c");
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, HALO_I2C0_BASE);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(armv7m, HALO_I2C0_IRQ));

    dev = qdev_new("designware-i2c");
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, HALO_I2C1_BASE);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(armv7m, HALO_I2C1_IRQ));

    /*
     * I2C1 display-path targets: the vga020 panel (16-bit register
     * addresses) and the TPS65132 rail PMIC (8-bit).  Both are plain
     * ack-everything register files — the display bring-up only needs
     * the transfers to succeed.
     */
    {
        I2CBus *i2c1 = I2C_BUS(qdev_get_child_bus(dev, "i2c-bus"));
        I2CSlave *tgt;

        tgt = i2c_slave_new("halo-i2c-regfile", HALO_VGA020_I2C_ADDR);
        qdev_prop_set_uint32(DEVICE(tgt), "addr-bytes", 2);
        i2c_slave_realize_and_unref(tgt, i2c1, &error_fatal);

        tgt = i2c_slave_new("halo-i2c-regfile", HALO_TPS65132_I2C_ADDR);
        qdev_prop_set_uint32(DEVICE(tgt), "addr-bytes", 1);
        i2c_slave_realize_and_unref(tgt, i2c1, &error_fatal);
    }

    /* CDC200 display controller: the UI window (scanline_0 IRQ only —
     * the driver's ISR is wired to it and resolves everything from
     * IRQ_STATUS0) */
    dev = qdev_new("halo-cdc200");
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, HALO_CDC200_BASE);
    sysbus_connect_irq(sbd, 0,
                       qdev_get_gpio_in(armv7m, HALO_CDC200_SCANLINE_IRQ));

    /* DSI host fake: PHY-lock/stop-state happy path for the panel
     * bring-up; its IRQ is wired but never raised */
    dev = qdev_new("halo-dsi");
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, HALO_DSI_BASE);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(armv7m, HALO_DSI_IRQ));

    /*
     * I2S0: the speaker.  Sinks the firmware's PCM stream to a WAV file
     * and/or the host audio backend, and — the part boot depends on —
     * raises IRQ 141 so the i2s_sync TX callback runs and
     * max98357a_audio_trigger_impl() stops burning its 5 s drain
     * timeout on the startup sound.
     */
    dev = qdev_new("halo-i2s");
    if (hms->i2s_wav_out) {
        qdev_prop_set_string(dev, "wav-out", hms->i2s_wav_out);
    }
    if (hms->audiodev) {
        qdev_prop_set_string(dev, "audiodev", hms->audiodev);
    }
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, HALO_I2S0_BASE);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(armv7m, HALO_I2S0_IRQ));
    hms->i2s_dev = dev;

    /*
     * dma2 (PL330) + LPPDM: the microphone.  Zephyr's dma_pl330.c
     * drives the standard debug interface (DBGINST0/1 + DBGCMD) and
     * builds DMAWFP/DMALDP/DMAST microcode, which QEMU's model
     * executes, so the upstream device is used as-is.
     *  - num_periph_req must cover request 30 (the LPPDM line);
     *  - PNS must allow it for non-secure channels, otherwise DMAWFP
     *    faults with PL330_FAULT_CH_PERIPH_ERR.
     */
    {
        DeviceState *dma;

        dma = qdev_new("pl330");
        qdev_prop_set_uint32(dma, "num_chnls", HALO_DMA2_CHANNELS);
        qdev_prop_set_uint8(dma, "num_periph_req", 32);
        qdev_prop_set_uint8(dma, "num_events", HALO_DMA2_NUM_EVENTS);
        qdev_prop_set_uint32(dma, "PNS", 0xFFFFFFFF);
        object_property_set_link(OBJECT(dma), "memory",
                                 OBJECT(get_system_memory()), &error_fatal);
        sbd = SYS_BUS_DEVICE(dma);
        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, HALO_DMA2_BASE);
        sysbus_connect_irq(sbd, 0,
                           qdev_get_gpio_in(armv7m, HALO_DMA2_ABORT_IRQ));
        for (unsigned ev = 0; ev < HALO_DMA2_CHANNELS; ev++) {
            sysbus_connect_irq(sbd, 1 + ev, qdev_get_gpio_in(armv7m, ev));
        }

        dev = qdev_new("halo-pdm");
        if (hms->pdm_wav_in) {
            qdev_prop_set_string(dev, "wav-in", hms->pdm_wav_in);
        }
        if (hms->audiodev) {
            qdev_prop_set_string(dev, "audiodev", hms->audiodev);
        }
        sbd = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, HALO_PDM_BASE);
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(armv7m, HALO_PDM_IRQ));
        qdev_connect_gpio_out_named(dev, "dma-req", 0,
                                    qdev_get_gpio_in(dma,
                                                     HALO_PDM_DMA_REQ));
        hms->pdm_dev = dev;
    }
}

/*
 * Runtime controls (ticket 0031): everything scriptable hangs off the
 * stable QOM path /machine as plain object properties, so the control
 * plane is generic QMP qom-get/qom-set — no QAPI schema additions:
 *
 *   button-pressed  (bool, rw)  the LPGPIO-1 button, true = held down
 *                               (active-low: drives the pin to 0)
 *   charger-connected (bool, rw) charger STAT on gpio1.3 — the pin is
 *                               physically high while charging (the
 *                               vbat driver double-inverts its
 *                               ACTIVE_LOW flag)
 *   charge-enabled  (bool, ro)  the firmware's charge-control output
 *                               (gpio0.6): false when driven low
 *   battery-raw     (uint32, rw) forwarded to the ADC model; the
 *                               firmware samples every 10 s
 *   led-duty / led-period (uint32, ro), led-on (bool, ro)
 *                               forwarded from the UTIMER3 PWM sink
 *   wdt-fire        (bool, wo-ish) writing true injects one watchdog
 *                               timeout (halo_wdog.c)
 *   speaker-enabled (bool, ro)  MAX98357A SD_MODE (gpio8.5)
 *   speaker-playing (bool, ro), speaker-rate / speaker-samples
 *                   (uint32, ro) forwarded from the I2S0 model
 *   mic-wav-in      (str, rw)   WAV file feeding the microphone
 *   mic-tone-hz / mic-tone-amplitude (uint32, rw) built-in test tone
 *   mic-source      (str, ro), mic-samples (uint32, ro)
 *
 * Interactive path: the 'B' key in the QEMU window presses/releases
 * the button (a plain keyboard handler — this machine has no other
 * keyboard device, so every UI key event lands here).
 */

static void halo_button_drive(HaloMachineState *hms, bool pressed)
{
    hms->button_pressed = pressed;
    qemu_set_irq(qdev_get_gpio_in_named(hms->lpgpio_dev, "in", 1), !pressed);
}

static bool halo_get_button(Object *obj, Error **errp)
{
    return HALO_MACHINE(obj)->button_pressed;
}

static void halo_set_button(Object *obj, bool value, Error **errp)
{
    HaloMachineState *hms = HALO_MACHINE(obj);

    if (!hms->lpgpio_dev) {
        error_setg(errp, "machine is not initialized yet");
        return;
    }
    halo_button_drive(hms, value);
}

static bool halo_get_charger(Object *obj, Error **errp)
{
    return HALO_MACHINE(obj)->charger_connected;
}

static void halo_set_charger(Object *obj, bool value, Error **errp)
{
    HaloMachineState *hms = HALO_MACHINE(obj);

    if (!hms->gpio1_dev) {
        error_setg(errp, "machine is not initialized yet");
        return;
    }
    hms->charger_connected = value;
    qemu_set_irq(qdev_get_gpio_in_named(hms->gpio1_dev, "in", 3), value);
}

static bool halo_get_charge_enabled(Object *obj, Error **errp)
{
    HaloMachineState *hms = HALO_MACHINE(obj);
    uint32_t dr, ddr;

    if (!hms->gpio0_dev) {
        error_setg(errp, "machine is not initialized yet");
        return false;
    }
    dr = object_property_get_uint(OBJECT(hms->gpio0_dev), "dr", errp);
    ddr = object_property_get_uint(OBJECT(hms->gpio0_dev), "ddr", errp);
    /* tri-stated input (or driven high) = charging allowed;
     * output-low = charging cut (alif_vbat.c charge_control()) */
    return !(ddr & (1u << 6)) || (dr & (1u << 6));
}

/* Forwarders to per-device properties (uint32 needs the visitor API).
 * The machine property names match the device property names, so the
 * getter can forward by `name`. */

static void halo_get_adc_uint32(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    HaloMachineState *hms = HALO_MACHINE(obj);
    uint32_t value = 0;

    if (hms->adc_dev) {
        value = object_property_get_uint(OBJECT(hms->adc_dev), name, errp);
    }
    visit_type_uint32(v, name, &value, errp);
}

static void halo_get_utimer_uint32(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    HaloMachineState *hms = HALO_MACHINE(obj);
    uint32_t value = 0;

    if (hms->utimer_dev) {
        value = object_property_get_uint(OBJECT(hms->utimer_dev), name,
                                         errp);
    }
    visit_type_uint32(v, name, &value, errp);
}

static void halo_get_i2s_uint32(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    HaloMachineState *hms = HALO_MACHINE(obj);
    uint32_t value = 0;

    if (hms->i2s_dev) {
        value = object_property_get_uint(OBJECT(hms->i2s_dev), name, errp);
    }
    visit_type_uint32(v, name, &value, errp);
}

static void halo_get_pdm_uint32(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    HaloMachineState *hms = HALO_MACHINE(obj);
    uint32_t value = 0;

    if (hms->pdm_dev) {
        value = object_property_get_uint(OBJECT(hms->pdm_dev), name, errp);
    }
    visit_type_uint32(v, name, &value, errp);
}

static void halo_set_pdm_uint32(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    HaloMachineState *hms = HALO_MACHINE(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (!hms->pdm_dev) {
        error_setg(errp, "machine is not initialized yet");
        return;
    }
    object_property_set_uint(OBJECT(hms->pdm_dev), name, value, errp);
}

static char *halo_get_mic_str(Object *obj, const char *name, Error **errp)
{
    HaloMachineState *hms = HALO_MACHINE(obj);

    if (!hms->pdm_dev) {
        return g_strdup("");
    }
    return object_property_get_str(OBJECT(hms->pdm_dev), name, errp);
}

static char *halo_get_mic_wav_in(Object *obj, Error **errp)
{
    return halo_get_mic_str(obj, "mic-wav-in", errp);
}

static void halo_set_mic_wav_in(Object *obj, const char *value, Error **errp)
{
    HaloMachineState *hms = HALO_MACHINE(obj);

    if (!hms->pdm_dev) {
        error_setg(errp, "machine is not initialized yet");
        return;
    }
    object_property_set_str(OBJECT(hms->pdm_dev), "mic-wav-in", value, errp);
}

static char *halo_get_mic_source(Object *obj, Error **errp)
{
    return halo_get_mic_str(obj, "mic-source", errp);
}

static bool halo_get_speaker_playing(Object *obj, Error **errp)
{
    HaloMachineState *hms = HALO_MACHINE(obj);

    return hms->i2s_dev &&
           object_property_get_bool(OBJECT(hms->i2s_dev), "speaker-playing",
                                    errp);
}

/* MAX98357A SD_MODE on gpio8.5: the firmware drives it high around a
 * playback (max98357a_audio_trigger_impl START/STOP), so it is the
 * amplifier's on/off state as a tester would see it. */
static bool halo_get_speaker_enabled(Object *obj, Error **errp)
{
    HaloMachineState *hms = HALO_MACHINE(obj);
    uint32_t dr, ddr;

    if (!hms->gpio8_dev) {
        error_setg(errp, "machine is not initialized yet");
        return false;
    }
    dr = object_property_get_uint(OBJECT(hms->gpio8_dev), "dr", errp);
    ddr = object_property_get_uint(OBJECT(hms->gpio8_dev), "ddr", errp);
    return (ddr & (1u << 5)) && (dr & (1u << 5));
}

static void halo_set_battery_raw(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    HaloMachineState *hms = HALO_MACHINE(obj);
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (!hms->adc_dev) {
        error_setg(errp, "machine is not initialized yet");
        return;
    }
    object_property_set_uint(OBJECT(hms->adc_dev), name, value, errp);
}

static bool halo_get_led_on(Object *obj, Error **errp)
{
    HaloMachineState *hms = HALO_MACHINE(obj);

    return hms->utimer_dev &&
           object_property_get_bool(OBJECT(hms->utimer_dev), "led-on", errp);
}

static bool halo_get_wdt_fire(Object *obj, Error **errp)
{
    HaloMachineState *hms = HALO_MACHINE(obj);

    return hms->wdog_dev &&
           object_property_get_bool(OBJECT(hms->wdog_dev), "fire", errp);
}

static void halo_set_wdt_fire(Object *obj, bool value, Error **errp)
{
    HaloMachineState *hms = HALO_MACHINE(obj);

    if (!hms->wdog_dev) {
        error_setg(errp, "machine is not initialized yet");
        return;
    }
    object_property_set_bool(OBJECT(hms->wdog_dev), "fire", value, errp);
}

static void halo_key_event(DeviceState *dev, QemuConsole *src,
                           QemuInputEvent *evt)
{
    HaloMachineState *hms = halo_machine;

    if (!hms || !hms->lpgpio_dev || evt->type != INPUT_EVENT_KIND_KEY) {
        return;
    }
    if (qemu_input_linux_to_qcode(evt->key.key) == Q_KEY_CODE_B) {
        halo_button_drive(hms, evt->key.down);
    }
}

static const QemuInputHandler halo_button_input_handler = {
    .name = "halo-button",
    .mask = INPUT_EVENT_MASK_KEY,
    .event = halo_key_event,
};

/*
 * BLE doorbell: transport between the synthetic ROM stub and the host.
 * Its chardev is -serial index 1 (the launcher exposes it as a TCP
 * socket); traps in the stub are decoded with ble-symfile.
 */
static void halo_create_ble(HaloMachineState *hms, DeviceState *armv7m)
{
    DeviceState *dev = qdev_new("halo-ble");
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    if (serial_hd(1)) {
        qdev_prop_set_chr(dev, "chardev", serial_hd(1));
    }
    if (hms->ble_symfile) {
        qdev_prop_set_string(dev, "symfile", hms->ble_symfile);
    }
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, HALO_BLE_DOORBELL_BASE);
    sysbus_mmio_map(sbd, 1, HALO_BLE_UTIMER0_BASE);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(armv7m, HALO_BLE_IRQ));
}

static void halo_init(MachineState *machine)
{
    HaloMachineState *hms = HALO_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    DeviceState *armv7m;

    /* Fixed-frequency clocks — no migration state needed */
    hms->sysclk = clock_new(OBJECT(machine), "SYSCLK");
    clock_set_hz(hms->sysclk, HALO_SYSCLK_FRQ);
    hms->refclk = clock_new(OBJECT(machine), "REFCLK");
    clock_set_hz(hms->refclk, HALO_REFCLK_FRQ);

    /*
     * MRAM: with mram-file=... it is a MAP_SHARED mmap of the host
     * file, so every guest store lands in the file — littlefs state
     * persists across runs with no explicit flush (the launcher
     * pre-sizes the file to exactly 2 MiB and injects the firmware
     * into the slot0 window before boot).
     */
    if (hms->mram_file) {
#ifdef CONFIG_POSIX
        memory_region_init_ram_from_file(&hms->mram, NULL, "halo.mram",
                                         HALO_MRAM_SIZE, 0, RAM_SHARED,
                                         hms->mram_file, 0, &error_fatal);
        memory_region_add_subregion(sysmem, HALO_MRAM_BASE, &hms->mram);
#else
        error_report("halo: mram-file= requires a POSIX host");
        exit(1);
#endif
    } else {
        halo_make_ram(&hms->mram, "halo.mram", HALO_MRAM_BASE,
                      HALO_MRAM_SIZE);
    }
    halo_make_ram(&hms->itcm, "halo.itcm", HALO_ITCM_BASE, HALO_ITCM_SIZE);
    halo_make_alias(&hms->itcm_alias, "halo.itcm.alias", &hms->itcm,
                    HALO_ITCM_ALIAS);
    halo_make_ram(&hms->romwin, "halo.romwin", HALO_ROMWIN_BASE,
                  HALO_ROMWIN_SIZE);
    halo_make_alias(&hms->romwin_alias, "halo.romwin.alias", &hms->romwin,
                    HALO_ROMWIN_ALIAS);
    /*
     * Fill the (empty) BLE/LC3 ROM window with Thumb `bx lr` so every
     * ROM entry point (ble_stack_init @ 0x140D55, rwip_* @ 0xBC0xx, ...)
     * returns immediately instead of executing zeros off the end of the
     * window (a fatal prefetch abort that kills the whole boot).  With
     * this, ble_task() parks in its event loop, the ROM init callback
     * never fires, and main() blocks in alif_ble_enable()'s
     * k_sem_take(K_FOREVER) — the documented pre-BLE end state.  The
     * synthetic-ROM ticket replaces these contents with a real stub.
     */
    {
        uint16_t *rom = memory_region_get_ram_ptr(&hms->romwin);

        for (hwaddr i = 0; i < HALO_ROMWIN_SIZE / 2; i++) {
            rom[i] = cpu_to_le16(0x4770); /* bx lr */
        }
    }

    /*
     * Synthetic BLE/LC3 ROM stub (rom-stub/, ticket 0028): overlay the
     * built image onto the window.  Without it the bx-lr fill keeps the
     * pre-0028 behavior (main() parks in alif_ble_enable()).
     */
    if (hms->rom_file) {
        gsize len = 0;
        g_autofree char *blob = NULL;
        GError *gerr = NULL;

        if (!g_file_get_contents(hms->rom_file, &blob, &len, &gerr)) {
            error_report("halo: cannot read rom-file: %s", gerr->message);
            exit(1);
        }
        if (len > HALO_ROMWIN_SIZE) {
            error_report("halo: rom-file is %zu bytes, exceeds the "
                         "0x%x ROM window", (size_t)len,
                         (unsigned)HALO_ROMWIN_SIZE);
            exit(1);
        }
        memcpy(memory_region_get_ram_ptr(&hms->romwin), blob, len);
    }
    halo_make_ram(&hms->dtcm, "halo.dtcm", HALO_DTCM_BASE, HALO_DTCM_SIZE);
    halo_make_alias(&hms->dtcm_alias, "halo.dtcm.alias", &hms->dtcm,
                    HALO_DTCM_ALIAS);
    qemu_register_reset(halo_sram_reset, hms);

    /*
     * Background coverage for everything the firmware pokes raw at boot
     * (CGU/AON/VBAT @ 0x1A6xxxxx, MHUv2 @ 0x4004/5xxxx, EVTRTR, LP
     * peripherals, UTIMER, expansion-master APBs @ 0x49xxxxxx, and the
     * PPB outside the NVIC: DWT, TGU page, ROM tables).  Real devices
     * mapped on top take precedence.
     */
    create_unimplemented_device("halo.cgu-aon", 0x1A000000, 0x01000000);
    create_unimplemented_device("halo.periph", 0x40000000, 0x10000000);

    /*
     * Boot-time clock/config scratch blocks whose read-modify-write
     * values must stick: EXPSLV (UART/I2C/ADC clock ctrl), EXPMST
     * (CDC200 pixclk ctrl — a read-as-zero divisor here is the display
     * driver's division by zero), M55HE config (per-core clock enables,
     * camera pixclk).  Plain RAM gives them register-file semantics.
     */
    halo_make_ram(&hms->expslv, "halo.expslv", 0x4902F000, 0x1000);
    halo_make_ram(&hms->expmst, "halo.expmst", 0x4903F000, 0x1000);
    halo_make_ram(&hms->m55he_cfg, "halo.m55he-cfg", 0x43007000, 0x1000);

    object_initialize_child(OBJECT(machine), "armv7m", &hms->armv7m,
                            TYPE_ARMV7M);
    armv7m = DEVICE(&hms->armv7m);
    qdev_prop_set_string(armv7m, "cpu-type", machine->cpu_type);
    qdev_prop_set_uint32(armv7m, "num-irq", HALO_NUM_IRQ);
    qdev_prop_set_uint8(armv7m, "num-prio-bits", 8);
    /*
     * Reset fetches MSP/PC from the vector table in MRAM — the app's
     * by default, 0x80000000 under the mcuboot chain-boot spike
     * (machine option svtor=).
     */
    qdev_prop_set_uint32(armv7m, "init-svtor", hms->init_svtor);
    qdev_connect_clock_in(armv7m, "cpuclk", hms->sysclk);
    qdev_connect_clock_in(armv7m, "refclk", hms->refclk);
    object_property_set_link(OBJECT(&hms->armv7m), "memory", OBJECT(sysmem),
                             &error_abort);
    sysbus_realize(SYS_BUS_DEVICE(&hms->armv7m), &error_fatal);

    /*
     * ITGU/DTGU: SystemInit reads CFG.BLKSZ and fills the LUTs.  They
     * live in the System PPB, which the armv7m container covers with a
     * whole-0xE00xxxxx RAZ/WI default region — system-memory mappings
     * are shadowed there, so map them into the container next to the
     * NVIC, at systick-style priority 1.
     */
    memory_region_init_io(&hms->itgu.iomem, OBJECT(machine), &halo_tgu_ops,
                          &hms->itgu, "halo.itgu", HALO_TGU_REGION_SIZE);
    memory_region_add_subregion_overlap(&hms->armv7m.container,
                                        HALO_ITGU_BASE, &hms->itgu.iomem, 1);
    memory_region_init_io(&hms->dtgu.iomem, OBJECT(machine), &halo_tgu_ops,
                          &hms->dtgu, "halo.dtgu", HALO_TGU_REGION_SIZE);
    memory_region_add_subregion_overlap(&hms->armv7m.container,
                                        HALO_DTGU_BASE, &hms->dtgu.iomem, 1);

    /*
     * Console UART3.  serial-mm covers the shifted 16550 register file
     * (0x00..0x1F); DesignWare-specific registers above it (DLF @ 0xC0)
     * land in halo.periph and are read-0/write-ignored, which the
     * Zephyr ns16550 driver tolerates.
     */
    serial_mm_init(sysmem, HALO_UART3_BASE, 2,
                   qdev_get_gpio_in(armv7m, HALO_UART3_IRQ),
                   HALO_UART3_CLK / 16, serial_hd(0), DEVICE_LITTLE_ENDIAN);

    /* SE fake + boot-critical peripheral models */
    halo_create_peripherals(hms, armv7m);

    /* BLE doorbell (synthetic ROM stub transport) */
    halo_create_ble(hms, armv7m);

    /* Runtime controls: singleton hook for halo_wdog.c + the 'B'-key
     * button binding in the UI window */
    halo_machine = hms;
    qemu_input_handler_activate(
        qemu_input_handler_register(NULL, &halo_button_input_handler));

    /*
     * Raw app image (zephyr.bin / zephyr.signed.bin) into MRAM slot 0.
     * With mram-file= the image is already in the backing file and
     * -kernel is omitted; a NULL filename still registers the CPU
     * reset handler (mandatory for M-profile).
     */
    armv7m_load_kernel(hms->armv7m.cpu, machine->kernel_filename,
                       HALO_APP_BASE, HALO_APP_MAX_SIZE);
}

static char *halo_get_mram_file(Object *obj, Error **errp)
{
    HaloMachineState *hms = HALO_MACHINE(obj);

    return g_strdup(hms->mram_file);
}

static void halo_set_mram_file(Object *obj, const char *value, Error **errp)
{
    HaloMachineState *hms = HALO_MACHINE(obj);

    g_free(hms->mram_file);
    hms->mram_file = g_strdup(value);
}

static char *halo_get_rom_file(Object *obj, Error **errp)
{
    HaloMachineState *hms = HALO_MACHINE(obj);

    return g_strdup(hms->rom_file);
}

static void halo_set_rom_file(Object *obj, const char *value, Error **errp)
{
    HaloMachineState *hms = HALO_MACHINE(obj);

    g_free(hms->rom_file);
    hms->rom_file = g_strdup(value);
}

static char *halo_get_ble_symfile(Object *obj, Error **errp)
{
    HaloMachineState *hms = HALO_MACHINE(obj);

    return g_strdup(hms->ble_symfile);
}

static void halo_set_ble_symfile(Object *obj, const char *value, Error **errp)
{
    HaloMachineState *hms = HALO_MACHINE(obj);

    g_free(hms->ble_symfile);
    hms->ble_symfile = g_strdup(value);
}

static char *halo_get_i2s_wav_out(Object *obj, Error **errp)
{
    HaloMachineState *hms = HALO_MACHINE(obj);

    return g_strdup(hms->i2s_wav_out);
}

static void halo_set_i2s_wav_out(Object *obj, const char *value, Error **errp)
{
    HaloMachineState *hms = HALO_MACHINE(obj);

    g_free(hms->i2s_wav_out);
    hms->i2s_wav_out = g_strdup(value);
}

static char *halo_get_pdm_wav_in(Object *obj, Error **errp)
{
    HaloMachineState *hms = HALO_MACHINE(obj);

    return g_strdup(hms->pdm_wav_in);
}

static void halo_set_pdm_wav_in(Object *obj, const char *value, Error **errp)
{
    HaloMachineState *hms = HALO_MACHINE(obj);

    g_free(hms->pdm_wav_in);
    hms->pdm_wav_in = g_strdup(value);
}

static char *halo_get_audiodev(Object *obj, Error **errp)
{
    HaloMachineState *hms = HALO_MACHINE(obj);

    return g_strdup(hms->audiodev);
}

static void halo_set_audiodev(Object *obj, const char *value, Error **errp)
{
    HaloMachineState *hms = HALO_MACHINE(obj);

    g_free(hms->audiodev);
    hms->audiodev = g_strdup(value);
}

static void halo_get_svtor(Object *obj, Visitor *v, const char *name,
                           void *opaque, Error **errp)
{
    HaloMachineState *hms = HALO_MACHINE(obj);

    visit_type_uint32(v, name, &hms->init_svtor, errp);
}

static void halo_set_svtor(Object *obj, Visitor *v, const char *name,
                           void *opaque, Error **errp)
{
    HaloMachineState *hms = HALO_MACHINE(obj);

    visit_type_uint32(v, name, &hms->init_svtor, errp);
}

static void halo_machine_instance_init(Object *obj)
{
    HaloMachineState *hms = HALO_MACHINE(obj);

    hms->init_svtor = HALO_APP_VTOR;
}

static void halo_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    static const char *const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-m55"),
        NULL
    };

    mc->desc = "Alif Balletto B1 (Cortex-M55 RTSS-HE) as used by the "
               "halo firmware";
    mc->init = halo_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-m55");
    mc->valid_cpu_types = valid_cpu_types;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;

    object_class_property_add_str(oc, "mram-file",
                                  halo_get_mram_file, halo_set_mram_file);
    object_class_property_set_description(oc, "mram-file",
        "Host file backing the 2 MiB MRAM (MAP_SHARED; persistent). "
        "Must be exactly 2 MiB. When set, omit -kernel: the firmware "
        "is expected in the file's slot0 window already");
    object_class_property_add_str(oc, "rom-file",
                                  halo_get_rom_file, halo_set_rom_file);
    object_class_property_set_description(oc, "rom-file",
        "Synthetic BLE/LC3 ROM stub image loaded at 0x00090000 "
        "(rom-stub/build/rom-stub-v1_2.bin). Without it the ROM window "
        "reads as bx-lr and boot parks in alif_ble_enable()");
    object_class_property_add_str(oc, "ble-symfile",
                                  halo_get_ble_symfile, halo_set_ble_symfile);
    object_class_property_set_description(oc, "ble-symfile",
        "ROM symbol table (rom-stub-v1_2.syms) used to decode stub trap "
        "reports into symbol names");
    object_class_property_add_str(oc, "i2s-wav-out",
                                  halo_get_i2s_wav_out,
                                  halo_set_i2s_wav_out);
    object_class_property_set_description(oc, "i2s-wav-out",
        "Write everything the firmware plays through I2S0 to this WAV "
        "file (16-bit mono, at the rate the guest programmed)");
    object_class_property_add_str(oc, "pdm-wav-in",
                                  halo_get_pdm_wav_in,
                                  halo_set_pdm_wav_in);
    object_class_property_set_description(oc, "pdm-wav-in",
        "Feed this WAV file to the microphone instead of silence "
        "(16-bit, mono or stereo, looped and resampled to whatever "
        "rate the firmware asks for)");
    object_class_property_add_str(oc, "audiodev",
                                  halo_get_audiodev, halo_set_audiodev);
    object_class_property_set_description(oc, "audiodev",
        "id of an -audiodev backend for live speaker playback and "
        "microphone capture; omit for a silent (file-only) machine");
    object_class_property_add(oc, "svtor", "uint32",
                              halo_get_svtor, halo_set_svtor, NULL, NULL);
    object_class_property_set_description(oc, "svtor",
        "Reset vector table address (default 0x80020800 = app slot0; "
        "0x80000000 for mcuboot chain-boot)");

    /* Runtime controls (ticket 0031) — driven over QMP qom-set/qom-get
     * by the halo-emu control socket */
    object_class_property_add_bool(oc, "button-pressed",
                                   halo_get_button, halo_set_button);
    object_class_property_set_description(oc, "button-pressed",
        "The hardware button (LPGPIO pin 1, active-low): true = held");
    object_class_property_add_bool(oc, "charger-connected",
                                   halo_get_charger, halo_set_charger);
    object_class_property_set_description(oc, "charger-connected",
        "Charger STAT input (gpio1.3): true = charging");
    object_class_property_add_bool(oc, "charge-enabled",
                                   halo_get_charge_enabled, NULL);
    object_class_property_set_description(oc, "charge-enabled",
        "Firmware charge-control output (gpio0.6): false = charge cut");
    object_class_property_add(oc, "battery-raw", "uint32",
                              halo_get_adc_uint32, halo_set_battery_raw,
                              NULL, NULL);
    object_class_property_set_description(oc, "battery-raw",
        "Battery ADC sample (0..4095); Vbat_mV = raw * 4320 / 4095");
    object_class_property_add(oc, "led-duty", "uint32",
                              halo_get_utimer_uint32, NULL, NULL, NULL);
    object_class_property_add(oc, "led-period", "uint32",
                              halo_get_utimer_uint32, NULL, NULL, NULL);
    object_class_property_add_bool(oc, "led-on",
                                   halo_get_led_on, NULL);
    object_class_property_set_description(oc, "led-on",
        "LED PWM driver enabled (UTIMER3 COMPARE_CTRL_A.DRIVER_EN)");
    object_class_property_add_bool(oc, "wdt-fire",
                                   halo_get_wdt_fire, halo_set_wdt_fire);
    object_class_property_set_description(oc, "wdt-fire",
        "Write true to inject one watchdog timeout (NMI + warm reset)");

    /* Audio readouts (ticket 0032) */
    object_class_property_add_bool(oc, "speaker-enabled",
                                   halo_get_speaker_enabled, NULL);
    object_class_property_set_description(oc, "speaker-enabled",
        "MAX98357A SD_MODE (gpio8.5): true = amplifier powered up");
    object_class_property_add_bool(oc, "speaker-playing",
                                   halo_get_speaker_playing, NULL);
    object_class_property_set_description(oc, "speaker-playing",
        "I2S0 TX is enabled and clocking samples out");
    object_class_property_add(oc, "speaker-rate", "uint32",
                              halo_get_i2s_uint32, NULL, NULL, NULL);
    object_class_property_set_description(oc, "speaker-rate",
        "Samples per second the I2S0 TX path is currently draining");
    object_class_property_add(oc, "speaker-samples", "uint32",
                              halo_get_i2s_uint32, NULL, NULL, NULL);
    object_class_property_set_description(oc, "speaker-samples",
        "Total samples handed to the speaker sink since boot");

    object_class_property_add_str(oc, "mic-wav-in",
                                  halo_get_mic_wav_in, halo_set_mic_wav_in);
    object_class_property_set_description(oc, "mic-wav-in",
        "WAV file feeding the microphone; set to \"\" to stop using one");
    object_class_property_add(oc, "mic-tone-hz", "uint32",
                              halo_get_pdm_uint32, halo_set_pdm_uint32,
                              NULL, NULL);
    object_class_property_set_description(oc, "mic-tone-hz",
        "Synthesise a sine of this frequency into the microphone "
        "(0 = off). A wav-in file takes precedence");
    object_class_property_add(oc, "mic-tone-amplitude", "uint32",
                              halo_get_pdm_uint32, halo_set_pdm_uint32,
                              NULL, NULL);
    object_class_property_set_description(oc, "mic-tone-amplitude",
        "Peak amplitude of the microphone tone (0..32767)");
    object_class_property_add(oc, "mic-samples", "uint32",
                              halo_get_pdm_uint32, NULL, NULL, NULL);
    object_class_property_set_description(oc, "mic-samples",
        "Total samples the microphone has pushed into the LPPDM FIFO");
    object_class_property_add_str(oc, "mic-source",
                                  halo_get_mic_source, NULL);
    object_class_property_set_description(oc, "mic-source",
        "Where microphone samples come from: wav, tone, host or silence");
}

static const TypeInfo halo_machine_types[] = {
    {
        .name = TYPE_HALO_MACHINE,
        .parent = TYPE_MACHINE,
        .instance_size = sizeof(HaloMachineState),
        .instance_init = halo_machine_instance_init,
        .class_init = halo_machine_class_init,
        .interfaces = arm_machine_interfaces,
    },
};

DEFINE_TYPES(halo_machine_types)
