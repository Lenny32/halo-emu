/*
 * stub_ipc.c — shared-ring transport to the QEMU doorbell device.
 * Layout and framing: halo_rom_ipc.h.
 */

#include "stub.h"

struct ring {
    volatile uint32_t head;
    volatile uint32_t tail;
    uint32_t rsvd[2];
    volatile uint8_t data[HALO_BLE_RING_DATA];
};

#define H2G ((struct ring *)HALO_BLE_H2G_RING_ADDR)
#define G2H ((struct ring *)HALO_BLE_G2H_RING_ADDR)

/* ROM header at the very start of the window: lets the QEMU device verify
 * that a stub image compatible with its IPC contract is actually loaded. */
struct rom_header {
    uint32_t magic;
    uint16_t version;
    uint16_t rsvd;
    uint32_t h2g_ring;
    uint32_t g2h_ring;
};

__attribute__((section(".romhdr"), used)) const struct rom_header rom_hdr = {
    .magic = HALO_ROM_HDR_MAGIC,
    .version = HALO_ROM_HDR_VERSION,
    .h2g_ring = HALO_BLE_H2G_RING_ADDR,
    .g2h_ring = HALO_BLE_G2H_RING_ADDR,
};

void stub_ipc_init(void)
{
    /* The H2G producer is the host; only reset our side's consumer view to
     * the producer position (drop anything stale), and fully own G2H. */
    H2G->tail = H2G->head;
    G2H->head = 0;
    G2H->tail = 0;
}

static uint32_t g2h_space(void)
{
    return HALO_BLE_RING_DATA - (G2H->head - G2H->tail);
}

static void g2h_put(const uint8_t *p, uint16_t n)
{
    uint32_t head = G2H->head;

    while (n--) {
        G2H->data[head & (HALO_BLE_RING_DATA - 1)] = *p++;
        head++;
    }
    G2H->head = head;
}

bool stub_ipc_send(uint8_t op, const void *hdr, uint16_t hdr_len,
                   const void *payload, uint16_t payload_len)
{
    uint16_t len = hdr_len + payload_len;
    uint8_t fhdr[4] = { op, 0, (uint8_t)(len & 0xFF), (uint8_t)(len >> 8) };
    unsigned spins = 0;

    stub_lock();
    while (g2h_space() < (uint32_t)(4 + len)) {
        /* Ring full: kick the doorbell so the host drains it.  The QEMU
         * device drains synchronously from the MMIO write (and its chardev
         * write blocks while the TCP client stalls), so this is the
         * backpressure path — notifications must not drop (ticket 0030).
         * The drain is a no-op only when no chardev client is attached, in
         * which case the bytes were discarded and space appears anyway;
         * the bound is a belt-and-braces guard against a wedged host. */
        stub_unlock();
        stub_ipc_kick();
        if (++spins > 1000000) {
            return false;
        }
        stub_lock();
    }
    g2h_put(fhdr, 4);
    if (hdr_len) {
        g2h_put(hdr, hdr_len);
    }
    if (payload_len) {
        g2h_put(payload, payload_len);
    }
    stub_unlock();
    return true;
}

void stub_ipc_kick(void)
{
    volatile uint32_t *mmio = (volatile uint32_t *)HALO_BLE_MMIO_BASE;

    mmio[HALO_BLE_REG_G2H_KICK / 4] = 1;
}

/* Pop one frame from the H2G ring.  Returns payload length and sets *op;
 * *op = 0 when the ring is empty.  Oversized payloads are truncated to
 * max_len but fully consumed. */
uint16_t stub_ipc_recv(uint8_t *op, uint8_t *payload, uint16_t max_len)
{
    uint32_t tail = H2G->tail;
    uint32_t avail = H2G->head - tail;
    uint8_t fhdr[4];
    uint16_t len;

    *op = 0;
    if (avail < 4) {
        return 0;
    }

    for (int i = 0; i < 4; i++) {
        fhdr[i] = H2G->data[(tail + i) & (HALO_BLE_RING_DATA - 1)];
    }
    len = (uint16_t)(fhdr[2] | (fhdr[3] << 8));
    if (avail < (uint32_t)(4 + len)) {
        return 0; /* frame still being written by the host */
    }

    for (uint16_t i = 0; i < len && i < max_len; i++) {
        payload[i] = H2G->data[(tail + 4 + i) & (HALO_BLE_RING_DATA - 1)];
    }
    H2G->tail = tail + 4 + len;

    *op = fhdr[0];
    return len < max_len ? len : max_len;
}
