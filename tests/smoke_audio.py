#!/usr/bin/env python3
"""smoke_audio.py — end-to-end gate of the audio paths (ticket 0032)
against a real firmware image.

Boots `halo-emu -f <firmware>` headless with a fresh MRAM image, a WAV
capture on the speaker and a generated tone on the microphone, then
checks all four halves of the ticket over the control socket
(tcp://127.0.0.1:9562) and the Lua REPL (tcp://127.0.0.1:9563):

    1. startup sound   the boot cue plays through I2S0 and lands in
                       --wav-out (the console also stops reporting
                       "speaker not ready")
    2. microphone      an injected tone comes back out of
                       frame.microphone.read() at the amplitude and
                       frequency it went in with
    3. speaker         frame.speaker.play() of generated PCM shows up in
                       the capture
    4. LC3             frame.microphone.start{encoder='lc3'} produces a
                       real bitstream that frame.speaker.start{
                       encoder='lc3'} decodes back to the same tone —
                       both directions through the ROM stub's liblc3

Note the microphone signal has to be a *tone*, not DC: the LPPDM runs
with its IIR/FIR bypassed but lua_microphone.c applies its own DC
blocker, so a constant level correctly arrives as zeros.

Usage: tests/smoke_audio.py [-f firmware.bin] [--repl-port 9563]
                            [--ctl-port 9562] [--keep-wav DIR]
Exit code 0 = all checks passed.
"""

import argparse
import math
import os
import struct
import subprocess
import sys
import tempfile
import time
import wave

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HALO_EMU = os.path.join(REPO, "halo-emu")
sys.path.insert(0, os.path.join(REPO, "tools"))
from repl_smoke import Repl  # noqa: E402
sys.path.insert(0, os.path.join(REPO, "tests"))
from smoke_controls import Ctl  # noqa: E402

TONE_HZ = 1000
TONE_AMP = 12000


def check(name, ok, detail=""):
    print(f"smoke_audio: {'PASS' if ok else 'FAIL'} — {name}"
          + (f" ({detail})" if detail else ""))
    if not ok:
        raise AssertionError(name)


def write_tone_wav(path, hz, amp, rate=16000, seconds=1):
    n = rate * seconds
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(struct.pack(
            "<%dh" % n,
            *[int(amp * math.sin(2 * math.pi * hz * i / rate))
              for i in range(n)]))


def read_wav(path):
    """(samples, rate) of a 16-bit mono WAV, ([], 0) if it does not exist
    yet — the model only creates it once the guest plays something."""
    if not os.path.exists(path):
        return [], 0
    with wave.open(path, "rb") as w:
        n = w.getnframes()
        if n == 0:
            return [], w.getframerate()
        return list(struct.unpack("<%dh" % n, w.readframes(n))), \
            w.getframerate()


def peak(samples):
    return max((abs(x) for x in samples), default=0)


def dominant_hz(samples, rate, window=3200):
    """Zero-crossing frequency estimate — enough to tell a 1 kHz tone
    from noise or from a wrongly-clocked playback.  Measured over the
    loudest window, because the driver pads the tail of every clip with
    silence blocks and those would drag the estimate down."""
    if len(samples) < 2 or not rate:
        return 0
    window = min(window, len(samples))
    starts = range(0, len(samples) - window + 1, max(1, window // 4))
    best = max(starts, key=lambda i: peak(samples[i:i + window],),
               default=0)
    seg = samples[best:best + window]
    crossings = sum(1 for a, b in zip(seg, seg[1:]) if (a < 0) != (b < 0))
    return crossings / 2 / (len(seg) / float(rate))


# Lua that summarises a capture without sending binary over the REPL:
# length, peak and zero-crossing count of a 16-bit LE sample string.
# Kept to a single line — the REPL evaluates each line as its own chunk,
# so a multi-line snippet fails with "'end' expected near <eof>".
LUA_SUMMARISE = (
    "local s=%s if s==nil or #s==0 then print('none') else "
    "local mx,zc,prev=0,0,0 "
    "for i=1,#s-1,2 do local v=string.byte(s,i)+256*string.byte(s,i+1) "
    "if v>=32768 then v=v-65536 end if v>mx then mx=v end "
    "if (v<0)~=(prev<0) then zc=zc+1 end prev=v end "
    "print(#s..' '..mx..' '..zc) end"
)


def summarise(r, expr, timeout=15):
    """-> (bytes, peak, zero_crossings) or None."""
    out = r.lua(LUA_SUMMARISE % expr, timeout=timeout)
    if not out or out.strip() == "none":
        return None
    try:
        n, mx, zc = (int(x) for x in out.split())
    except ValueError:
        return None
    return n, mx, zc


def main():
    p = argparse.ArgumentParser()
    p.add_argument("-f", "--firmware",
                   default=os.path.join(REPO, "0.8.8.bin"))
    p.add_argument("--repl-port", type=int, default=9563)
    p.add_argument("--ctl-port", type=int, default=9562)
    p.add_argument("--boot-wait", type=float, default=25.0)
    p.add_argument("--keep-wav", metavar="DIR",
                   help="keep the capture/injection WAVs in DIR")
    args = p.parse_args()

    if not os.path.exists(args.firmware):
        sys.exit(f"smoke_audio: firmware not found: {args.firmware}")

    workdir = args.keep_wav or tempfile.mkdtemp(prefix="halo-audio-smoke-")
    os.makedirs(workdir, exist_ok=True)
    flash = os.path.join(workdir, "mram.img")
    spk_wav = os.path.join(workdir, "speaker.wav")
    mic_wav = os.path.join(workdir, "mic.wav")
    write_tone_wav(mic_wav, TONE_HZ, TONE_AMP)

    proc = subprocess.Popen(
        [HALO_EMU, "-f", args.firmware, "--flash", flash, "--flash-erase",
         "--headless", "--repl-port", str(args.repl_port),
         "--ctl-port", str(args.ctl_port),
         "--wav-in", mic_wav, "--wav-out", spk_wav],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    ok = False
    try:
        time.sleep(args.boot_wait)
        if proc.poll() is not None:
            sys.exit(f"smoke_audio: halo-emu exited early ({proc.returncode})")

        ctl = Ctl(args.ctl_port)
        check("ctl ping", ctl.cmd("ping") == "ok")

        # --- 1. the boot cue reached the speaker -----------------------
        boot_samples, rate = read_wav(spk_wav)
        check("startup sound captured", len(boot_samples) > 1000,
              f"{len(boot_samples)} samples @ {rate} Hz")
        check("startup sound is not silence", peak(boot_samples) > 1000,
              f"peak {peak(boot_samples)}")

        reply = ctl.cmd("speaker?")
        fields = dict(kv.split("=") for kv in reply.split()[1:])
        check("speaker? readout",
              {"enabled", "playing", "rate", "samples"} <= set(fields),
              reply)
        check("speaker drained samples", int(fields["samples"]) > 1000,
              reply)

        r = Repl(args.repl_port)
        time.sleep(1.0)
        r.send(0, b"\x03")   # break main.lua so its output stays quiet
        time.sleep(0.5)
        r.drain()

        # --- 2. injected tone comes back out of the microphone ---------
        reply = ctl.cmd("mic?")
        check("mic? reports the injected file", "source=wav" in reply, reply)

        out = r.lua("frame.microphone.start{sample_rate=16000, bit_depth=16, "
                    "channels=1} print('mic')")
        check("microphone start", out == "mic", repr(out))
        time.sleep(2.0)   # past the driver's 20 ms start-of-stream discard
        # read() drains a FIFO from the *start* of the stream, not live
        # audio, so the first window always lands in the firmware's
        # start-of-stream ramp: consecutive 10 ms windows measure
        # 4383 -> 7297 -> 10901 -> 12017 and then hold steady. Measuring
        # the first window made this check flaky (it passed or failed on
        # where the ramp fell inside it), so drain the ramp first. The
        # frequency is correct throughout — only the level ramps.
        got = None
        for _ in range(8):
            got = summarise(r, "frame.microphone.read(320)")
            if got is None or got[1] > TONE_AMP * 0.75:
                break
        check("microphone returns samples", got is not None)
        n, mx, zc = got
        check("microphone sample count", n == 320, f"{n} bytes")
        # the DC blocker and gain stage move the level a little, so allow
        # a generous window around the injected amplitude
        check("microphone amplitude matches the injection",
              TONE_AMP * 0.5 < mx < TONE_AMP * 1.5, f"peak {mx}")
        hz = zc / 2 / (160 / 16000.0)
        check("microphone frequency matches the injection",
              TONE_HZ * 0.8 < hz < TONE_HZ * 1.2, f"~{hz:.0f} Hz")
        r.lua("frame.microphone.stop() print('off')")
        r.drain(0.5)

        # --- 3. PCM playback reaches the capture -----------------------
        before = int(dict(kv.split("=")
                          for kv in ctl.cmd("speaker?").split()[1:])["samples"])
        out = r.lua("frame.speaker.start{sample_rate=16000, bit_depth=16, "
                    "channels=1} frame.speaker.volume(100) "
                    "local t={} for i=0,1599 do "
                    "  local v=math.floor(10000*math.sin(2*math.pi*1000*i/16000)) "
                    "  if v<0 then v=v+65536 end "
                    "  t[#t+1]=string.char(v%256, math.floor(v/256)) end "
                    "frame.speaker.play(table.concat(t)) print('played')",
                    timeout=20)
        check("speaker play", out == "played", repr(out))
        time.sleep(2.0)
        after = int(dict(kv.split("=")
                         for kv in ctl.cmd("speaker?").split()[1:])["samples"])
        check("speaker consumed the PCM", after - before >= 1600,
              f"{after - before} samples")
        r.lua("frame.speaker.stop() print('off')")
        r.drain(0.5)

        played, rate = read_wav(spk_wav)
        tail = played[len(boot_samples):]
        check("PCM playback is in the capture", peak(tail) > 1000,
              f"peak {peak(tail)} over {len(tail)} samples")

        # --- 4. LC3 both ways through the ROM stub ---------------------
        out = r.lua("frame.microphone.start{encoder='lc3', sample_rate=16000, "
                    "channels=1, bitrate=32000} print('mic')")
        check("microphone start (LC3)", out == "mic", repr(out))
        time.sleep(2.5)
        out = r.lua("LC3='' while #LC3 < 800 do "
                    "local s=frame.microphone.read(800-#LC3) "
                    "if s==nil or #s==0 then break end LC3=LC3..s end "
                    "print(#LC3)", timeout=20)
        check("LC3 encoder produced a bitstream", out == "800", repr(out))
        r.lua("frame.microphone.stop() print('off')")
        r.drain(0.5)

        before = int(dict(kv.split("=")
                          for kv in ctl.cmd("speaker?").split()[1:])["samples"])
        mark = len(read_wav(spk_wav)[0])
        out = r.lua("frame.speaker.start{encoder='lc3', sample_rate=16000, "
                    "bit_depth=16, channels=1, bitrate=32000} "
                    "frame.speaker.volume(100) "
                    "frame.speaker.play(LC3) print('played')", timeout=20)
        check("speaker play (LC3)", out == "played", repr(out))
        time.sleep(2.0)
        after = int(dict(kv.split("=")
                         for kv in ctl.cmd("speaker?").split()[1:])["samples"])
        # 800 B / 40 B per frame = 20 frames of 10 ms at 16 kHz = 3200
        check("LC3 decoder produced PCM", after - before >= 3200,
              f"{after - before} samples")
        r.lua("frame.speaker.stop() print('off')")

        decoded, rate = read_wav(spk_wav)
        seg = decoded[mark:]
        check("LC3 round trip is audible", peak(seg) > 1000,
              f"peak {peak(seg)}")
        hz = dominant_hz(seg, rate)
        check("LC3 round trip preserved the tone",
              TONE_HZ * 0.8 < hz < TONE_HZ * 1.2, f"~{hz:.0f} Hz")

        print("smoke_audio: PASS — all checks green")
        ok = True
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        if not args.keep_wav:
            for f in (flash, spk_wav, mic_wav):
                if os.path.exists(f):
                    os.unlink(f)
            os.rmdir(workdir)
        else:
            print(f"smoke_audio: WAVs kept in {workdir}")
        if not ok:
            print("smoke_audio: FAIL", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
