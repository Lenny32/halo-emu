/*
 * stub_core.c — stack lifecycle: ble_stack_init / rwip_init / rwip_process,
 * plus the tiny freestanding runtime (memcpy/memset, locking, RNG, trap
 * reporting).
 */

#include "stub.h"

struct stub_state g_stub;

/* ------------------------------------------------------------------ */
/* Freestanding runtime                                                */
/* ------------------------------------------------------------------ */

void *stub_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *stub_memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;

    while (n--) {
        *d++ = (uint8_t)c;
    }
    return dst;
}

/* GCC may emit calls to these even with -ffreestanding */
void *memcpy(void *dst, const void *src, size_t n)
{
    return stub_memcpy(dst, src, n);
}

void *memset(void *dst, int c, size_t n)
{
    return stub_memset(dst, c, n);
}

/* liblc3 (stub_lc3.c) shifts sample histories in place */
void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    if (d == s || n == 0) {
        return dst;
    }
    if (d < s) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dst;
}

void stub_lock(void)
{
    if (g_stub.hooks && g_stub.hooks->p_global_int_disable) {
        g_stub.hooks->p_global_int_disable();
    }
}

void stub_unlock(void)
{
    if (g_stub.hooks && g_stub.hooks->p_global_int_restore) {
        g_stub.hooks->p_global_int_restore();
    }
}

uint32_t hstub_co_rand_word(void)
{
    /* xorshift32 — deterministic, seeded once; quality is irrelevant here */
    uint32_t x = g_stub.rand_state ? g_stub.rand_state : 0x48414C4Fu;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_stub.rand_state = x;
    return x;
}

/* ------------------------------------------------------------------ */
/* Trap reporting (unimplemented pinned symbol was called)             */
/* ------------------------------------------------------------------ */

void rom_stub_trap_report(uint32_t idx, uint32_t lr)
{
    volatile uint32_t *mmio = (volatile uint32_t *)HALO_BLE_MMIO_BASE;

    mmio[HALO_BLE_REG_TRAP_LR / 4] = lr;
    mmio[HALO_BLE_REG_TRAP_IDX / 4] = idx; /* this write logs on the host */
}

__attribute__((naked)) void rom_stub_trap_entry(void)
{
    /* r12 = symbol index (set by the generated thunk), lr = caller.
     * Report to the doorbell device, then return GAP_ERR_NOT_SUPPORTED
     * (0x42) so callers that check a uint8/uint16 status see a failure
     * instead of a silent bogus success. */
    __asm__ volatile("push {r0-r3, r12, lr}\n" /* 24 bytes: keeps SP 8-aligned */
                     "mov r0, r12\n"
                     "mov r1, lr\n"
                     "bl rom_stub_trap_report\n"
                     "pop {r0-r3, r12, lr}\n"
                     "movs r0, #0x42\n"
                     "movs r1, #0\n"
                     "bx lr\n");
}

/* ------------------------------------------------------------------ */
/* H2G notification path: sync-timer capture callback -> BLE task      */
/* ------------------------------------------------------------------ */

static void stub_h2g_evt_cb(void)
{
    /* IRQ context (firmware sync-timer capture ISR).  Just schedule the
     * BLE task; the ring is drained from rwip_process(). */
    if (g_stub.hooks && g_stub.hooks->p_rtos_evt_post) {
        g_stub.hooks->p_rtos_evt_post();
    }
}

/* ------------------------------------------------------------------ */
/* Pinned entry points                                                 */
/* ------------------------------------------------------------------ */

/*
 * Note on the return value: ble_api.h documents ble_init_err_code_t
 * (0 = BLE_INIT_ERR_NONE = success), but the firmware-side consumer
 * (modules/hal/alif/ble/plf/alif_ble.c) does
 *     bool ble_success = ble_stack_init(...); if (!ble_success) ASSERT;
 * i.e. it treats the result as a boolean where nonzero = success — which is
 * how the real ROM behaves.  Match the real ROM, not the header comment.
 */
uint32_t hstub_ble_stack_init(const ble_app_hooks_t *hooks,
                              const ble_rom_config_t *cfg)
{
    if (hooks == NULL || cfg == NULL) {
        return 0; /* failure */
    }

    /* No crt0 runs in a ROM: zero the whole .data/.bss region (see
     * rom-stub.ld MEMORY DATA) before touching any state. */
    stub_memset((void *)0x0014E000, 0, 0x8000);
    g_stub.hooks = hooks;
    g_stub.next_hdl = STUB_FIRST_HDL;
    stub_cobuf_init();
    stub_ipc_init();

    return 1; /* success */
}

void hstub_rwip_init(uint32_t error)
{
    (void)error;

    if (g_stub.hooks == NULL) {
        return;
    }

    /* Register our capture callback with the firmware's sync-timer driver
     * so the doorbell IRQ (UTIMER0 capture A) reaches the stub.  The
     * firmware only stores the callbacks when both are non-NULL. */
    if (g_stub.hooks->p_sync_timer_start) {
        g_stub.hooks->p_sync_timer_start(stub_h2g_evt_cb, stub_h2g_evt_cb);
    }

    /* Stack is "initialised": release alif_ble_enable(). */
    if (g_stub.hooks->p_app_init) {
        g_stub.hooks->p_app_init();
    }
}

void hstub_rwip_process(void)
{
    uint8_t op;
    /* Static: rwip_process is only ever run by the single BLE task, and the
     * BLE thread stack is too small for a buffer this size.  Must hold a
     * GATT_WRITE frame for a full 512-byte ATT payload plus its 2-byte
     * handle prefix (ticket 0030: MTU 512). */
    static uint8_t payload[2 + 512];

    for (;;) {
        uint16_t len = stub_ipc_recv(&op, payload, sizeof(payload));

        if (op == 0) {
            break;
        }

        switch (op) {
        case HALO_BLE_OP_CONNECT:
            if (len >= 7) {
                stub_conn_dispatch_connect(payload);
            }
            break;
        case HALO_BLE_OP_DISCONNECT:
            stub_conn_dispatch_disconnect(
                len >= 2 ? (uint16_t)(payload[0] | (payload[1] << 8))
                         : 0x13 /* remote user terminated */);
            break;
        case HALO_BLE_OP_GATT_WRITE:
            if (len >= 2) {
                stub_gatt_host_write(
                    (uint16_t)(payload[0] | (payload[1] << 8)),
                    payload + 2, (uint16_t)(len - 2));
            }
            break;
        case HALO_BLE_OP_GATT_READ:
            if (len >= 2) {
                stub_gatt_host_read(
                    (uint16_t)(payload[0] | (payload[1] << 8)));
            }
            break;
        case HALO_BLE_OP_ASE_CODEC:
        case HALO_BLE_OP_ASE_QOS:
        case HALO_BLE_OP_ASE_ENABLE:
        case HALO_BLE_OP_ASE_START:
        case HALO_BLE_OP_ASE_DISABLE:
        case HALO_BLE_OP_ASE_RELEASE:
        case HALO_BLE_OP_ASE_DP:
            /* LE Audio: the fabricated central (ticket 0038) */
            stub_ase_host_op(op, payload, len);
            break;
        case HALO_BLE_OP_ISO_SDU:
            /* One isochronous SDU from the host (ticket 0039) */
            stub_iso_host_sdu(payload, len);
            break;
        default:
            break;
        }
    }
}
