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
 *    watchdog stub (never fires), DW APB RTC (the tickless-idle
 *    counter), Alif ADC12 with a configurable battery voltage, UTIMER3
 *    as a PWM sink for the LED, eleven DW GPIO banks, and two DW I2C
 *    controllers backed by QEMU I2C buses (no targets yet — absent
 *    addresses NACK cleanly).
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
#include "hw/misc/unimp.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "qom/object.h"

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
#define HALO_ROMWIN_SIZE 0x000D0000 /* ..0x00160000 */
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
    uint32_t init_svtor;
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
} halo_gpio_banks[] = {
    { "gpio0", 0x49000000, 179, 8 },
    { "gpio1", 0x49001000, 187, 8 },
    { "gpio2", 0x49002000, 195, 8 },
    { "gpio3", 0x49003000, 203, 8 },
    { "gpio4", 0x49004000, 211, 8 },
    { "gpio5", 0x49005000, 219, 8 },
    { "gpio6", 0x49006000, 227, 8 },
    { "gpio7", 0x49007000, 235, 8 },
    { "gpio8", 0x49008000, 243, 8 },
    { "gpio9", 0x49009000, 251, 3 },
    { "lpgpio", 0x42002000, 171, 2 },
};

static void halo_create_peripherals(DeviceState *armv7m)
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

    /* watchdog stub: armed and fed by the firmware, never fires */
    dev = qdev_new("halo-wdog");
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, HALO_WDOG_BASE);

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

    /* UTIMER3 PWM sink (LED duty cycle recorder) */
    dev = qdev_new("halo-utimer");
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, HALO_UTIMER3_BASE);
    sysbus_mmio_map(sbd, 1, HALO_UTIMER_GLB_BASE);

    for (unsigned i = 0; i < ARRAY_SIZE(halo_gpio_banks); i++) {
        dev = qdev_new("halo-dwgpio");
        qdev_prop_set_uint32(dev, "ngpios", halo_gpio_banks[i].ngpios);
        sbd = SYS_BUS_DEVICE(dev);
        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, halo_gpio_banks[i].base);
        for (unsigned pin = 0; pin < halo_gpio_banks[i].ngpios; pin++) {
            sysbus_connect_irq(sbd, pin,
                qdev_get_gpio_in(armv7m, halo_gpio_banks[i].irq_base + pin));
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
    halo_make_ram(&hms->dtcm, "halo.dtcm", HALO_DTCM_BASE, HALO_DTCM_SIZE);
    halo_make_alias(&hms->dtcm_alias, "halo.dtcm.alias", &hms->dtcm,
                    HALO_DTCM_ALIAS);

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
    halo_create_peripherals(armv7m);

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
    object_class_property_add(oc, "svtor", "uint32",
                              halo_get_svtor, halo_set_svtor, NULL, NULL);
    object_class_property_set_description(oc, "svtor",
        "Reset vector table address (default 0x80020800 = app slot0; "
        "0x80000000 for mcuboot chain-boot)");
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
