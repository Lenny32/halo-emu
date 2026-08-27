#!/usr/bin/env python3
"""smoke_le_audio.py — end-to-end gate of standards-based LE Audio
(tickets 0038 and 0039) against a real firmware image.

Boots `halo-emu -f <firmware>` headless with a tone fed to the
microphone, then drives the BAP unicast server from the control socket
(tcp://127.0.0.1:9562) the way a central would:

    1. init clean         halo_ble_audio_init() completes — the boot log
                          no longer carries "Unable to configure BAP
                          unicast server! Error 255" (ticket 0028 scoped
                          the whole GAF tier as unsupported; 0038
                          implements it)
    2. source ASE         codec -> qos -> enable -> start walks ASE 2
                          (the source characteristic) to STREAMING, each
                          transition reported back over the doorbell
    3. LC3 out            the firmware encodes microphone audio and ships
                          SDUs to the host: 40-octet frames, the BAP 16_2
                          size the codec was configured for
    4. sink ASE           ASE 0 (a sink characteristic) to STREAMING
    5. LC3 in             those same SDUs replayed into the sink datapath
                          are decoded and reach the speaker
    6. release            back to IDLE

There is no HCI or link layer in the emulator (see EMULATOR.md), so the
peer is fabricated host-side; what is real is the firmware's half of the
conversation — its request callbacks run and its confirmations drive the
state machine.

ASE local indices follow the firmware's own split (sink characteristics
first): with sink=2 and src=1 it advertises, lids 0 and 1 are sinks and
lid 2 is the source.

Usage: tests/smoke_le_audio.py [-f firmware.bin] [--repl-port 9563]
                               [--ctl-port 9562]
Exit code 0 = all checks passed.
"""

import argparse
import os
import subprocess
import sys
import tempfile
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HALO_EMU = os.path.join(REPO, "halo-emu")
sys.path.insert(0, os.path.join(REPO, "tools"))
sys.path.insert(0, os.path.join(REPO, "tests"))
from repl_smoke import Repl  # noqa: E402
from smoke_controls import Ctl  # noqa: E402
from smoke_audio import write_tone_wav, TONE_HZ, TONE_AMP  # noqa: E402

SINK_ASE = 0
SOURCE_ASE = 2
# BAP 16_2: 16 kHz, 10 ms, 40 octets per frame.
FRAME_OCTET = 40


def check(name, ok, detail=""):
    print(f"smoke_le_audio: {'PASS' if ok else 'FAIL'} — {name}"
          + (f" ({detail})" if detail else ""))
    if not ok:
        raise AssertionError(name)


def fields(reply):
    return dict(kv.split("=", 1) for kv in reply.split()[1:] if "=" in kv)


def settle(ctl, lid, want, tries=24, gap=0.25):
    """Wait for an ASE to report a state; transitions arrive asynchronously
    over the doorbell, so a read straight after the command races it."""
    for _ in range(tries):
        reply = ctl.cmd(f"ase? {lid}")
        if reply.split("=", 1)[-1] == want:
            return True, reply
        time.sleep(gap)
    return False, ctl.cmd(f"ase? {lid}")


def walk_to_streaming(ctl, lid):
    """codec -> qos -> enable -> start, checking each hop."""
    for step, want in (("codec", "codec-configured"),
                       ("qos", "qos-configured"),
                       ("enable", "enabling"),
                       ("start", "streaming")):
        ctl.cmd(f"ase {step} {lid}")
        ok, reply = settle(ctl, lid, want)
        check(f"ASE {lid}: {step} -> {want}", ok, reply)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("-f", "--firmware",
                   default=os.path.join(REPO, "0.8.8.bin"))
    p.add_argument("--repl-port", type=int, default=9563)
    p.add_argument("--ctl-port", type=int, default=9562)
    p.add_argument("--boot-wait", type=float, default=22.0)
    args = p.parse_args()

    if not os.path.exists(args.firmware):
        sys.exit(f"smoke_le_audio: firmware not found: {args.firmware}")

    workdir = tempfile.mkdtemp(prefix="halo-leaudio-smoke-")
    mic_wav = os.path.join(workdir, "mic.wav")
    write_tone_wav(mic_wav, TONE_HZ, TONE_AMP)

    boot_log = os.path.join(workdir, "boot.log")
    proc = subprocess.Popen(
        [HALO_EMU, "-f", args.firmware,
         "--flash", os.path.join(workdir, "mram.img"), "--headless",
         "--repl-port", str(args.repl_port),
         "--ctl-port", str(args.ctl_port),
         "--wav-in", mic_wav,
         "--wav-out", os.path.join(workdir, "speaker.wav")],
        stdout=open(boot_log, "w"), stderr=subprocess.STDOUT)
    ok = False
    try:
        time.sleep(args.boot_wait)
        if proc.poll() is not None:
            sys.exit(f"smoke_le_audio: halo-emu exited early "
                     f"({proc.returncode})")

        ctl = Ctl(args.ctl_port)
        # Connecting to the REPL port makes the bridge open a link and
        # pair, which is what the ASE state machine hangs off.
        r = Repl(args.repl_port)
        time.sleep(2.0)

        check("ctl ping", ctl.cmd("ping") == "ok")

        # 1. LE Audio init completed: no configure failure in the log
        log = open(boot_log, errors="replace").read()
        check("halo_ble_audio_init() completed",
              "Unable to configure BAP unicast server" not in log
              and "Failed to initialize LE Audio service" not in log)
        check("no ROM stub trap during LE Audio init",
              "ROM stub trap" not in log)

        # 2+3. source ASE to streaming, and LC3 SDUs coming out
        ctl.cmd("iso clear")
        walk_to_streaming(ctl, SOURCE_ASE)

        got = None
        for _ in range(20):
            got = fields(ctl.cmd("iso?"))
            if int(got["sdus"]) > 20:
                break
            time.sleep(0.25)
        check("firmware streams LC3 SDUs on the source ASE",
              int(got["sdus"]) > 20, str(got))
        check("SDUs are the configured BAP 16_2 frame size",
              int(got["last"]) == FRAME_OCTET, str(got))
        check("SDU payload accounting is consistent",
              int(got["bytes"]) == int(got["sdus"]) * FRAME_OCTET, str(got))

        # 4+5. sink ASE to streaming, replayed SDUs reach the speaker
        walk_to_streaming(ctl, SINK_ASE)

        before = fields(ctl.cmd("speaker?"))
        replayed = ctl.cmd("iso replay 100")
        check("replayed the captured SDUs", "replayed=100" in replayed,
              replayed)
        time.sleep(2.0)
        after = fields(ctl.cmd("speaker?"))
        check("decoded LC3 reached the speaker",
              int(after["samples"]) > int(before["samples"]) + 1000,
              f"{before['samples']} -> {after['samples']}")

        # The decode runs on the firmware's 3 KB ble_audio_dec thread; a
        # liblc3 built for its default 96 kHz profile overflows it and
        # halts the guest, so prove the guest is still alive.
        log = open(boot_log, errors="replace").read()
        check("guest survived the decode (no stack overflow)",
              "ZEPHYR FATAL ERROR" not in log)
        check("REPL still answers after streaming",
              r.lua("print('alive')") == "alive")

        # 6. release
        for lid in (SOURCE_ASE, SINK_ASE):
            ctl.cmd(f"ase release {lid}")
            released, reply = settle(ctl, lid, "idle")
            check(f"ASE {lid}: release -> idle", released, reply)

        print("smoke_le_audio: PASS — all checks green")
        ok = True
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
