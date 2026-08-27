/*
 * halo_rom_ipc.h — shared contract between the synthetic BLE ROM stub
 * (rom-stub/) and the QEMU doorbell device (patches/files/hw/arm/halo_ble.c).
 *
 * KEEP IN SYNC with patches/files/hw/arm/halo_rom_ipc.h (identical copy —
 * the QEMU tree cannot include files from outside the fork).
 *
 * Transport: two byte rings in the ROM window RAM, plus a doorbell MMIO
 * page and one NVIC line.
 *
 *  - Host->guest (H2G): the QEMU device writes frames into the H2G ring and
 *    pulses NVIC IRQ 377 (UTIMER0 capture A — the IRQ the firmware's BLE
 *    sync-timer driver has already connected).  The firmware ISR calls the
 *    capture callback the stub registered through the app hook table, which
 *    posts the BLE task; the stub drains the ring from rwip_process().
 *  - Guest->host (G2H): the stub writes frames into the G2H ring and writes
 *    the G2H_KICK doorbell register; the device forwards the frames to its
 *    chardev (halo-emu exposes it as a TCP socket).
 *
 * Ring: { u32 head; u32 tail; u32 rsvd[2]; u8 data[HALO_BLE_RING_DATA]; }
 * head/tail are free-running byte counters (index = counter & (DATA-1)).
 * Producer advances head, consumer advances tail.
 *
 * Frame (both rings and the chardev stream, all little-endian):
 *   u8 op, u8 flags (0), u16 len, u8 payload[len]
 */

#ifndef HALO_ROM_IPC_H_
#define HALO_ROM_IPC_H_

/* ROM window */
#define HALO_ROM_BASE 0x00090000u
#define HALO_ROM_HDR_ADDR HALO_ROM_BASE
#define HALO_ROM_HDR_MAGIC 0x4D4F5248u /* "HROM" */
#define HALO_ROM_HDR_VERSION 0x0102u   /* ROM symbol map v1_2 */

/* Shared rings (inside the ROM window RAM) */
#define HALO_BLE_H2G_RING_ADDR 0x00156000u
#define HALO_BLE_G2H_RING_ADDR 0x0015A000u
#define HALO_BLE_RING_DATA 0x2000u /* power of two */
#define HALO_BLE_RING_OFF_HEAD 0u
#define HALO_BLE_RING_OFF_TAIL 4u
#define HALO_BLE_RING_OFF_DATA 16u

/* Doorbell MMIO page */
#define HALO_BLE_MMIO_BASE 0x4904E000u
#define HALO_BLE_REG_MAGIC 0x00u   /* RO: HALO_ROM_HDR_MAGIC */
#define HALO_BLE_REG_G2H_KICK 0x04u /* WO: drain G2H ring to chardev */
#define HALO_BLE_REG_TRAP_LR 0x08u  /* WO: caller LR of a trapped ROM call */
#define HALO_BLE_REG_TRAP_IDX 0x0Cu /* WO: symbol index; write logs the trap */

/* NVIC line used for H2G notification: UTIMER0 capture A (sync_timer.c) */
#define HALO_BLE_H2G_IRQ 377u

/* Frame opcodes: host -> guest */
#define HALO_BLE_OP_CONNECT 0x01u     /* u8 addr_type, u8 addr[6] */
#define HALO_BLE_OP_DISCONNECT 0x02u  /* u16 reason */
#define HALO_BLE_OP_GATT_WRITE 0x03u  /* u16 hdl, u8 data[] */
#define HALO_BLE_OP_GATT_READ 0x04u   /* u16 hdl */

/* LE Audio: the fabricated central driving the ASE state machine
 * (ticket 0038).  There is no HCI here, so these stand in for the ASE
 * Control Point writes a real central would make. */
#define HALO_BLE_OP_ASE_CODEC 0x05u   /* u8 ase_lid, con_lid, tgt_latency,
                                       * tgt_phy, sampling_freq, frame_dur,
                                       * u16 frame_octet */
#define HALO_BLE_OP_ASE_QOS 0x06u     /* u8 ase_lid, u8 stream_lid */
#define HALO_BLE_OP_ASE_ENABLE 0x07u  /* u8 ase_lid */
#define HALO_BLE_OP_ASE_START 0x08u   /* u8 ase_lid (CIS established) */
#define HALO_BLE_OP_ASE_DISABLE 0x09u /* u8 ase_lid */
#define HALO_BLE_OP_ASE_RELEASE 0x0Au /* u8 ase_lid */
#define HALO_BLE_OP_ASE_DP 0x0Bu      /* u8 ase_lid, u8 start */
#define HALO_BLE_OP_ISO_SDU 0x0Cu     /* u8 stream_lid, u8 rfu, u16 seq,
                                       * u8 lc3[] (ticket 0039) */

/* Frame opcodes: guest -> host */
#define HALO_BLE_EVT_NOTIFY 0x81u     /* u16 hdl, u8 evt_type, u8 data[] */
#define HALO_BLE_EVT_SVC 0x82u        /* u16 start_hdl, u8 nb_att, u8 uuid[16] */
#define HALO_BLE_EVT_ATT 0x83u        /* u16 hdl, u16 info, u8 uuid[16] */
#define HALO_BLE_EVT_CONNECTED 0x84u  /* u8 conidx */
#define HALO_BLE_EVT_DISCONNECTED 0x85u /* u8 conidx, u16 reason */
#define HALO_BLE_EVT_WRITE_STATUS 0x86u /* u16 hdl, u16 status */
#define HALO_BLE_EVT_READ_RSP 0x87u   /* u16 hdl, u16 status, u8 data[] */
#define HALO_BLE_EVT_PAIRED 0x88u     /* u8 conidx */
#define HALO_BLE_EVT_ADV_STATE 0x89u  /* u8 actv_idx, u8 state (see below) */
#define HALO_BLE_EVT_ADV_DATA 0x8Au   /* u8 actv_idx, u8 kind, u8 data[] */
#define HALO_BLE_EVT_ASE_STATE 0x8Bu  /* u8 ase_lid, u8 con_lid, u8 state */
#define HALO_BLE_EVT_ISO_SDU 0x8Cu    /* u8 stream_lid, u8 rfu, u16 seq,
                                       * u8 lc3[] (ticket 0039) */

#define HALO_BLE_ADV_CREATED 0u
#define HALO_BLE_ADV_STARTED 1u
#define HALO_BLE_ADV_STOPPED 2u

#define HALO_BLE_ADV_DATA_ADV 0u
#define HALO_BLE_ADV_DATA_SCAN_RSP 1u

#endif /* HALO_ROM_IPC_H_ */
