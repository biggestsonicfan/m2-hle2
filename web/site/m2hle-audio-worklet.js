/*
 * The audio half of the web build that runs on the browser's AUDIO thread.
 *
 * The page posts chunks of interleaved stereo float frames, already at the
 * context's sample rate (audio_out_drain in src/core/audio_out.h renders them).
 * This keeps them in a ring and plays 128 at a time. The ring is the ONLY queue
 * between the sound board and the device, so its depth is the latency we choose,
 * and the reason this exists: see "The push model" in audio_out.h.
 *
 * What the depth has to cover is how unevenly chunks arrive. The board makes 735
 * frames per 60 Hz slice and the page runs a slice per display frame, so chunks
 * land every ~16.7 ms -- unless the page drops a frame, and then nothing lands
 * for ~33 ms and two arrive together. `target` is sized for that.
 *
 *   buffering  Nothing plays until the ring holds `target` frames. Without this
 *              the cushion would have to be built by the 1% rate nudge, which
 *              takes seconds, and every start would stutter.
 *   underrun   The ring ran dry: hold the last level and let it decay (no click),
 *              go back to buffering. A short silence, then a clean restart.
 *   resync     A ring far past its target is stale, not early (the page was
 *              hidden, the tab stalled): drop the oldest down to the target.
 *
 * It reports its fill back to the page, which low-passes it and steers the
 * resampling ratio so the two clocks stay together.
 */
const CAPACITY = 16384;           /* frames */
const REPORT_EVERY = 16;          /* quanta: ~46 ms at 44.1 kHz */
const FADE_IN = 128;              /* frames */

class M2hleOut extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const o = (options && options.processorOptions) || {};
    this.target = Math.max(256, Math.min(o.target | 0 || 1792, CAPACITY / 2));
    this.ring = new Float32Array(CAPACITY * 2);
    this.r = 0;
    this.fill = 0;
    this.buffering = true;
    this.fade = 0;                /* frames of fade-in still to play */
    this.lastL = 0;
    this.lastR = 0;
    this.underruns = 0;
    this.resyncs = 0;
    this.quanta = 0;
    this.port.onmessage = (e) => this.push(e.data);
  }

  drop(frames) {
    this.r = (this.r + frames) % CAPACITY;
    this.fill -= frames;
  }

  push(chunk) {
    const frames = chunk.length >> 1;
    if (frames > CAPACITY) return;
    if (this.fill + frames > CAPACITY) this.drop(this.fill + frames - CAPACITY);
    let w = (this.r + this.fill) % CAPACITY;
    for (let i = 0; i < frames; i++) {
      this.ring[w * 2] = chunk[i * 2];
      this.ring[w * 2 + 1] = chunk[i * 2 + 1];
      w = (w + 1) % CAPACITY;
    }
    this.fill += frames;
    if (this.fill > this.target * 3) {
      this.drop(this.fill - this.target);
      this.fade = FADE_IN;
      this.resyncs++;
    }
  }

  process(inputs, outputs) {
    const out = outputs[0];
    const L = out[0];
    const R = out.length > 1 ? out[1] : null;
    const n = L.length;

    if (this.buffering && this.fill >= this.target) {
      this.buffering = false;
      this.fade = FADE_IN;
    }
    if (!this.buffering && this.fill < n) {
      this.buffering = true;
      this.underruns++;
    }

    if (this.buffering) {
      /* Decay from the last level instead of stepping to zero: a step is a click. */
      for (let i = 0; i < n; i++) {
        this.lastL *= 0.995;
        this.lastR *= 0.995;
        L[i] = this.lastL;
        if (R) R[i] = this.lastR;
      }
    } else {
      let r = this.r;
      for (let i = 0; i < n; i++) {
        let l = this.ring[r * 2];
        let rr = this.ring[r * 2 + 1];
        if (this.fade > 0) {
          const g = 1 - this.fade / FADE_IN;
          l = this.lastL + (l - this.lastL) * g;
          rr = this.lastR + (rr - this.lastR) * g;
          this.fade--;
        }
        L[i] = l;
        if (R) R[i] = rr;
        this.lastL = l;
        this.lastR = rr;
        r = (r + 1) % CAPACITY;
      }
      this.r = r;
      this.fill -= n;
    }

    if (++this.quanta >= REPORT_EVERY) {
      this.quanta = 0;
      this.port.postMessage({
        fill: this.fill, buffering: this.buffering,
        underruns: this.underruns, resyncs: this.resyncs,
      });
    }
    return true;
  }
}

registerProcessor('m2hle-out', M2hleOut);
