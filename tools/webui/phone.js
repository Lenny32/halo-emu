// phone.js — capture a mic + camera and stream them to the av_bridge
// over /ws/phone (ticket 0040), with device pickers so the page works
// as well from a laptop as from a phone.  Wire format: JSON hello, then
// tagged binary frames: 0x01 + s16le PCM, 0x02 + <HH> w,h + RGB888.
// "Listen to emulator" plays /ws/speaker through the selected output
// device (AudioContext.setSinkId where the browser supports it).

'use strict';

const $ = (id) => document.getElementById(id);
const statusEl = $('status');
const video = $('preview');
const meterBar = document.querySelector('#meter > div');

let ws = null;
let stream = null;
let actx = null;
let camTimer = null;
let running = false;
let sentFrames = 0;
let sentBytes = 0;

const canvas = document.createElement('canvas');
const c2d = canvas.getContext('2d', { willReadFrequently: true });

function setStatus(text, on) {
  statusEl.textContent = text;
  statusEl.className = 'badge' + (on ? ' on' : '');
}

function camSize() {
  const [w, h] = $('res').value.split('x').map(Number);
  return { w, h };
}

// --- device pickers --------------------------------------------------------

async function updateDevices() {
  let devs;
  try {
    devs = await navigator.mediaDevices.enumerateDevices();
  } catch (_) {
    return;
  }
  const fill = (sel, kind, label) => {
    const keep = sel.value;
    sel.innerHTML = '<option value="">(default)</option>';
    let n = 0;
    for (const d of devs) {
      if (d.kind !== kind) continue;
      n++;
      const o = document.createElement('option');
      o.value = d.deviceId;
      // Labels are blank until a getUserMedia grant; show a generic
      // name so the picker is still usable before the first Start.
      o.textContent = d.label || `${label} ${n}`;
      sel.appendChild(o);
    }
    if ([...sel.options].some((o) => o.value === keep)) sel.value = keep;
  };
  fill($('cam-dev'), 'videoinput', 'camera');
  fill($('mic-dev'), 'audioinput', 'microphone');
  fill($('out-dev'), 'audiooutput', 'output');
}

// --- capture ---------------------------------------------------------------

async function start() {
  $('start').disabled = true;
  try {
    setStatus('requesting mic + camera…');
    const camId = $('cam-dev').value;
    const micId = $('mic-dev').value;
    stream = await navigator.mediaDevices.getUserMedia({
      audio: Object.assign(
        { channelCount: 1, echoCancellation: true,
          noiseSuppression: true },
        micId ? { deviceId: { exact: micId } } : {}),
      video: Object.assign(
        { width: { ideal: 640 }, height: { ideal: 480 } },
        camId ? { deviceId: { exact: camId } }
              : { facingMode: 'environment' }),
    });
    video.srcObject = stream;
    await video.play();
    await updateDevices(); // permission granted: labels are real now

    // 16 kHz is a hint; iOS ignores it and the bridge resamples.
    actx = new AudioContext({ sampleRate: 16000 });
    await actx.resume();
    await actx.audioWorklet.addModule('/webui/worklets.js');
    const src = actx.createMediaStreamSource(stream);
    const mic = new AudioWorkletNode(actx, 'mic-capture');
    src.connect(mic); // deliberately not connected to the speakers

    ws = new WebSocket(`wss://${location.host}/ws/phone`);
    ws.binaryType = 'arraybuffer';
    ws.onopen = () => {
      const { w, h } = camSize();
      const fps = Number($('fps').value);
      ws.send(JSON.stringify({ hello: {
        mic: { rate: actx.sampleRate },
        cam: { w, h, fps },
      }}));
      camTimer = setInterval(sendFrame, 1000 / fps);
      running = true;
      setStatus(`streaming (mic ${actx.sampleRate} Hz)`, true);
      $('start').textContent = 'Stop';
      $('start').classList.remove('primary');
      $('start').disabled = false;
    };
    ws.onclose = ws.onerror = () => { if (running) stop('disconnected'); };

    mic.port.onmessage = (e) => {
      const f32 = e.data;
      let peak = 0;
      const out = new Uint8Array(1 + f32.length * 2);
      out[0] = 0x01;
      // s16 payload starts at offset 1, unaligned for Int16Array, so
      // write through a DataView.
      const dv = new DataView(out.buffer);
      for (let i = 0; i < f32.length; i++) {
        const v = Math.max(-1, Math.min(1, f32[i]));
        peak = Math.max(peak, Math.abs(v));
        dv.setInt16(1 + i * 2, (v * 32767) | 0, true);
      }
      meterBar.style.width = Math.min(100, peak * 140) + '%';
      if (ws && ws.readyState === WebSocket.OPEN) ws.send(out);
    };
  } catch (err) {
    setStatus('failed: ' + err.message);
    $('start').disabled = false;
    cleanup();
  }
}

function sendFrame() {
  if (!ws || ws.readyState !== WebSocket.OPEN || video.readyState < 2)
    return;
  const { w, h } = camSize();
  canvas.width = w;
  canvas.height = h;
  c2d.drawImage(video, 0, 0, w, h);
  const rgba = c2d.getImageData(0, 0, w, h).data;
  const out = new Uint8Array(5 + w * h * 3);
  out[0] = 0x02;
  out[1] = w & 255; out[2] = w >> 8;
  out[3] = h & 255; out[4] = h >> 8;
  let o = 5;
  for (let i = 0; i < rgba.length; i += 4) {
    out[o++] = rgba[i];
    out[o++] = rgba[i + 1];
    out[o++] = rgba[i + 2];
  }
  ws.send(out);
  sentFrames++;
  sentBytes += out.length;
  $('stats').textContent =
    `${sentFrames} frames, ${(sentBytes / 1e6).toFixed(1)} MB sent`;
}

function cleanup() {
  if (camTimer) clearInterval(camTimer);
  camTimer = null;
  if (ws) { try { ws.close(); } catch (_) {} ws = null; }
  if (actx) { actx.close(); actx = null; }
  if (stream) {
    stream.getTracks().forEach((t) => t.stop());
    stream = null;
  }
  video.srcObject = null;
}

function stop(reason) {
  running = false;
  cleanup();
  setStatus(reason || 'stopped');
  $('start').textContent = 'Start streaming';
  $('start').classList.add('primary');
  $('start').disabled = false;
}

async function restartIfRunning() {
  if (!running) return;
  stop('switching device…');
  await start();
}

// --- listen to the emulator's speaker --------------------------------------

let listenCtx = null;
let listenWs = null;

async function applySink() {
  const id = $('out-dev').value;
  if (!listenCtx) return;
  if (typeof listenCtx.setSinkId !== 'function') {
    if (id) $('listen-state').textContent =
      'live (output picker unsupported in this browser)';
    return;
  }
  try {
    await listenCtx.setSinkId(id ? id : '');
  } catch (err) {
    $('listen-state').textContent = 'sink failed: ' + err.message;
  }
}

async function toggleListen() {
  if (listenCtx) {
    if (listenWs) { try { listenWs.close(); } catch (_) {} }
    return; // onclose does the teardown
  }
  $('listen').disabled = true;
  // Created inside the click handler so autoplay policy lets it run.
  listenCtx = new AudioContext();
  await listenCtx.resume();
  await listenCtx.audioWorklet.addModule('/webui/worklets.js');
  const player = new AudioWorkletNode(listenCtx, 'pcm-player');
  player.connect(listenCtx.destination);
  await applySink();

  let srcRate = 32000;
  listenWs = new WebSocket(`wss://${location.host}/ws/speaker`);
  listenWs.binaryType = 'arraybuffer';
  listenWs.onmessage = (e) => {
    if (typeof e.data === 'string') {
      const o = JSON.parse(e.data);
      srcRate = o.rate || 32000;
      $('listen-state').textContent = o.enabled
        ? `live @ ${srcRate} Hz` : 'no --wav-out (attach mode)';
      $('listen-state').className = 'badge' + (o.enabled ? ' on' : '');
      return;
    }
    const i16 = new Int16Array(e.data);
    const ratio = listenCtx.sampleRate / srcRate;
    const n = Math.floor(i16.length * ratio);
    const f32 = new Float32Array(n);
    for (let i = 0; i < n; i++) {
      const pos = i / ratio;
      const j = pos | 0;
      const frac = pos - j;
      const a = i16[j];
      const b = i16[Math.min(j + 1, i16.length - 1)];
      f32[i] = (a + (b - a) * frac) / 32768;
    }
    player.port.postMessage(f32, [f32.buffer]);
  };
  listenWs.onclose = listenWs.onerror = () => {
    if (listenCtx) listenCtx.close();
    listenCtx = null;
    listenWs = null;
    $('listen-state').textContent = 'off';
    $('listen-state').className = 'badge';
    $('listen').textContent = '🔊 Listen to emulator';
    $('listen').disabled = false;
  };
  $('listen').textContent = '🔇 Stop listening';
  $('listen').disabled = false;
}

// --- wire up ---------------------------------------------------------------

$('start').addEventListener('click', () => (running ? stop() : start()));
$('listen').addEventListener('click', toggleListen);
$('cam-dev').addEventListener('change', restartIfRunning);
$('mic-dev').addEventListener('change', restartIfRunning);
$('out-dev').addEventListener('change', applySink);
navigator.mediaDevices.addEventListener('devicechange', updateDevices);
updateDevices();
