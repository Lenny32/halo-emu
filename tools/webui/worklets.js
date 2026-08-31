// AudioWorklet processors for the av_bridge pages (ticket 0040).
//
//   mic-capture  (phone page)   batches mic input into 2048-sample
//                               Float32 blocks posted to the page
//   pcm-player   (desktop page) plays queued Float32 blocks; silence on
//                               underrun (the guest's virtual clock can
//                               lag wall time), drops oldest past ~0.75 s
//                               when the guest catches up in a burst

class MicCapture extends AudioWorkletProcessor {
  constructor() {
    super();
    this.buf = new Float32Array(2048);
    this.n = 0;
  }

  process(inputs) {
    const ch = inputs[0][0];
    if (ch) {
      let off = 0;
      while (off < ch.length) {
        const take = Math.min(ch.length - off, this.buf.length - this.n);
        this.buf.set(ch.subarray(off, off + take), this.n);
        this.n += take;
        off += take;
        if (this.n === this.buf.length) {
          this.port.postMessage(this.buf.slice(0));
          this.n = 0;
        }
      }
    }
    return true;
  }
}

class PcmPlayer extends AudioWorkletProcessor {
  constructor() {
    super();
    this.queue = [];
    this.queued = 0; // samples across the queue, minus this.offset
    this.offset = 0; // consumed from queue[0]
    this.max = sampleRate * 0.75;
    this.port.onmessage = (e) => {
      this.queue.push(e.data);
      this.queued += e.data.length;
      while (this.queued > this.max && this.queue.length > 1) {
        this.queued -= this.queue[0].length - this.offset;
        this.queue.shift();
        this.offset = 0;
      }
    };
  }

  process(_inputs, outputs) {
    const out = outputs[0][0];
    let i = 0;
    while (i < out.length && this.queue.length) {
      const head = this.queue[0];
      const take = Math.min(out.length - i, head.length - this.offset);
      out.set(head.subarray(this.offset, this.offset + take), i);
      i += take;
      this.offset += take;
      this.queued -= take;
      if (this.offset === head.length) {
        this.queue.shift();
        this.offset = 0;
      }
    }
    for (; i < out.length; i++) out[i] = 0;
    return true;
  }
}

registerProcessor('mic-capture', MicCapture);
registerProcessor('pcm-player', PcmPlayer);
