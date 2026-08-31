// app.js — desktop UI for the av_bridge (ticket 0040): panel mirror,
// control-verb pass-through, phone onboarding, live speaker playback.

'use strict';

const $ = (id) => document.getElementById(id);
const wsUrl = (path) => `wss://${location.host}${path}`;

// --- control channel -----------------------------------------------------

let ctlWs = null;
let nextId = 1;
const pending = new Map();

function log(line) {
  const el = $('log');
  el.textContent += line + '\n';
  el.scrollTop = el.scrollHeight;
}

function ctl(line) {
  return new Promise((resolve) => {
    if (!ctlWs || ctlWs.readyState !== WebSocket.OPEN) {
      log('! bridge not connected');
      resolve('err not connected');
      return;
    }
    const id = nextId++;
    pending.set(id, resolve);
    log('> ' + line);
    ctlWs.send(JSON.stringify({ ctl: line, id }));
  });
}

function bridgeCmd(obj) {
  if (ctlWs && ctlWs.readyState === WebSocket.OPEN)
    ctlWs.send(JSON.stringify(obj));
}

const STATUS_ROWS = [
  ['battery', (s) => s.battery &&
    `${s.battery.pct}% (${s.battery.mv} mV)`],
  ['charger', (s) => s.charger &&
    (s.charger.connected === '1' ? 'connected' : 'unplugged')],
  ['led', (s) => s.led &&
    (s.led.on === '1' ? `on (duty ${s.led.duty}/${s.led.period})` : 'off')],
  ['mic', (s) => s.mic &&
    `${s.mic.source} (${s.mic.samples} samples)`],
  ['mic buffer', (s) => s.bridge && `${s.bridge.mic_buffer_ms} ms`],
  ['camera', (s) => s.camera &&
    `${s.camera.source} ${s.camera.size} captures=${s.camera.captures}` +
    (s.camera.streaming === '1' ? ' streaming' : '')],
  ['frames pushed', (s) => s.bridge && `${s.bridge.camera_pushed}`],
  ['speaker', (s) => s.speaker &&
    (s.speaker.playing === '1'
      ? `playing @ ${s.speaker.rate} Hz` : 'idle')],
];

function renderStatus(s) {
  const kv = $('status-kv');
  kv.innerHTML = '';
  for (const [name, fn] of STATUS_ROWS) {
    let v;
    try { v = fn(s); } catch (_) { v = null; }
    const k = document.createElement('span');
    k.className = 'k';
    k.textContent = name;
    const val = document.createElement('span');
    val.textContent = v || '—';
    kv.append(k, val);
  }
  const phone = $('phone-state');
  phone.textContent = s.bridge && s.bridge.phone
    ? 'phone connected' : 'no phone';
  phone.className = 'badge' + (s.bridge && s.bridge.phone ? ' on' : '');
  if (s.bridge && s.bridge.phone_url)
    $('phone-url').textContent = s.bridge.phone_url;
  if (s.charger)
    $('charger').checked = s.charger.connected === '1';
}

function connectControl() {
  ctlWs = new WebSocket(wsUrl('/ws/control'));
  ctlWs.onmessage = (e) => {
    const obj = JSON.parse(e.data);
    if (obj.status) renderStatus(obj.status);
    else if (obj.event) log('* ' + obj.event);
    else if (obj.reply !== undefined) {
      log('< ' + obj.reply);
      const resolve = pending.get(obj.id);
      if (resolve) { pending.delete(obj.id); resolve(obj.reply); }
    }
  };
  ctlWs.onclose = () => setTimeout(connectControl, 1500);
}

// --- display mirror --------------------------------------------------------

function connectDisplay() {
  const c2d = $('screen').getContext('2d');
  const ws = new WebSocket(wsUrl('/ws/display'));
  ws.binaryType = 'arraybuffer';
  ws.onmessage = (e) => {
    const dv = new DataView(e.data);
    const w = dv.getUint32(0, true);
    const h = dv.getUint32(4, true);
    const rgb = new Uint8Array(e.data, 8);
    const img = c2d.createImageData(w, h);
    const px = img.data;
    for (let i = 0, o = 0; o < px.length; i += 3, o += 4) {
      px[o] = rgb[i];
      px[o + 1] = rgb[i + 1];
      px[o + 2] = rgb[i + 2];
      px[o + 3] = 255;
    }
    $('screen').width = w;
    $('screen').height = h;
    c2d.putImageData(img, 0, 0);
  };
  ws.onclose = () => setTimeout(connectDisplay, 1500);
}

// --- speaker ---------------------------------------------------------------

let spkOn = false;

async function enableSpeaker() {
  if (spkOn) return;
  spkOn = true;
  // Created inside the click handler so autoplay policy lets it run.
  const actx = new AudioContext();
  await actx.resume();
  await actx.audioWorklet.addModule('/webui/worklets.js');
  const player = new AudioWorkletNode(actx, 'pcm-player');
  player.connect(actx.destination);

  let srcRate = 32000;
  const ws = new WebSocket(wsUrl('/ws/speaker'));
  ws.binaryType = 'arraybuffer';
  ws.onmessage = (e) => {
    if (typeof e.data === 'string') {
      const o = JSON.parse(e.data);
      srcRate = o.rate || 32000;
      $('spk-state').textContent = o.enabled
        ? `speaker live @ ${srcRate} Hz` : 'no --wav-out (attach mode)';
      $('spk-state').className = 'badge' + (o.enabled ? ' on' : '');
      return;
    }
    const i16 = new Int16Array(e.data);
    const ratio = actx.sampleRate / srcRate;
    const n = Math.floor(i16.length * ratio);
    const f32 = new Float32Array(n);
    if (ratio === 1) {
      for (let i = 0; i < n; i++) f32[i] = i16[i] / 32768;
    } else {
      for (let i = 0; i < n; i++) {
        const pos = i / ratio;
        const j = pos | 0;
        const frac = pos - j;
        const a = i16[j];
        const b = i16[Math.min(j + 1, i16.length - 1)];
        f32[i] = (a + (b - a) * frac) / 32768;
      }
    }
    player.port.postMessage(f32, [f32.buffer]);
  };
  ws.onclose = () => {
    spkOn = false;
    $('spk-state').textContent = 'speaker off';
    $('spk-state').className = 'badge';
    actx.close();
  };
}

// --- wire up ---------------------------------------------------------------

document.querySelectorAll('[data-ctl]').forEach((b) =>
  b.addEventListener('click', () => ctl(b.dataset.ctl)));

$('btn-click').onclick = () => ctl('button click');
$('btn-hold').onclick = () => ctl('button hold 1000');
$('btn-down').onclick = () => ctl('button down');
$('btn-up').onclick = () => ctl('button up');

$('batt').oninput = () => { $('batt-lbl').textContent = $('batt').value + '%'; };
$('batt').onchange = () => ctl(`battery set ${$('batt').value}%`);
$('charger').onchange = () =>
  ctl(`charger ${$('charger').checked ? 'on' : 'off'}`);
$('accel-set').onclick = () =>
  ctl(`accel ${$('ax').value} ${$('ay').value} ${$('az').value}`);
$('shutdown').onclick = () => {
  if (confirm('Shut down the emulator (QEMU quit)?'))
    bridgeCmd({ cmd: 'shutdown', id: nextId++ });
};
$('cam-fps').onchange = () =>
  bridgeCmd({ cmd: 'camera-fps', fps: Number($('cam-fps').value) });

$('ctl-send').onclick = () => {
  const line = $('ctl-line').value.trim();
  if (line) { ctl(line); $('ctl-line').value = ''; }
};
$('ctl-line').addEventListener('keydown', (e) => {
  if (e.key === 'Enter') $('ctl-send').click();
});
$('spk-enable').onclick = enableSpeaker;

connectControl();
connectDisplay();
