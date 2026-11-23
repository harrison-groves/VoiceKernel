const BLOCK = 128;

class AudioKernelProcessor extends AudioWorkletProcessor {
    constructor(options) {
        super();

        const opts = options.processorOptions;

        this.mode = opts.mode || 'passthrough';
        this.chunkSize = Math.floor(
            (opts.sampleRate || 48000) * ((opts.chunkDuration || 250) / 1000)
        );
        this.gain = typeof opts.gain === 'number' ? opts.gain : 1.0;

        this.exp = null;
        this.kernel = 0;
        this.inputView = null;
        this.outputView = null;
        this.isReady = false;

        this.accumulator = new Float32Array(this.chunkSize);
        this.accumulatorIndex = 0;

        // Stub imports so an older build (with ALLOW_MEMORY_GROWTH) still loads.
        const imports = { env: { emscripten_notify_memory_growth: () => {} } };

        WebAssembly.instantiate(opts.wasmBytes, imports).then(({ instance }) => {
            const exp = instance.exports;
            if (exp._initialize) exp._initialize();   // run C++ static initialisers

            this.exp = exp;
            this.kernel = exp.create_kernel();
            exp.set_parameter(this.kernel, 0, this.gain);
            this._attachViews();
            this.isReady = true;

            const latencySamples = exp.get_latency_samples
                ? exp.get_latency_samples(this.kernel)
                : 0;
            this.port.postMessage({ type: 'ready', latencySamples });
        }).catch((err) => {
            this.port.postMessage({ type: 'error', message: String(err && err.message || err) });
        });

        this.port.onmessage = (event) => {
            const { type, id, value, mode, chunkDuration, sampleRate } = event.data;

            if (type === 'param' && id === 0) {
                this.gain = value;
                if (this.isReady) this.exp.set_parameter(this.kernel, 0, value);
            }

            if (type === 'setMode') {
                if (this.accumulatorIndex > 0 && this.mode !== 'passthrough') {
                    this._postChunk(this.accumulatorIndex);   // don't drop a partial chunk
                }
                this.mode = mode;
                this.accumulatorIndex = 0;
                if (chunkDuration && sampleRate) {
                    this.chunkSize = Math.floor(sampleRate * (chunkDuration / 1000));
                    this.accumulator = new Float32Array(this.chunkSize);
                }
            }

            if (type === 'stats' && this.isReady && this.exp.get_underrun_count) {
                this.port.postMessage({
                    type: 'stats',
                    underruns: this.exp.get_underrun_count(this.kernel),
                });
            }
        };
    }

    _attachViews() {
        const buf = this.exp.memory.buffer;
        this.inputView = new Float32Array(buf, this.exp.get_input_ptr(), BLOCK);
        this.outputView = new Float32Array(buf, this.exp.get_output_ptr(), BLOCK);
    }

    process(inputs, outputs) {
        const input = inputs[0][0];
        const out = outputs[0];

        if (!input || !out[0]) return true;

        // Silent while the WASM loads (a few ms). The kernel fades in once ready.
        if (!this.isReady) return true;

        // Views go stale if WASM memory ever grows (not expected with static memory)
        if (this.inputView.buffer !== this.exp.memory.buffer) this._attachViews();

        this.inputView.set(input);
        this.exp.process_audio(this.kernel, this.inputView.byteOffset,
                               this.outputView.byteOffset, input.length);

        // The kernel already applied gain, so output and chunks always match.
        out[0].set(this.outputView.subarray(0, input.length));
        for (let ch = 1; ch < out.length; ch++) out[ch].set(out[0]);

        if (this.mode === 'accumulate' || this.mode === 'both') {
            this._accumulate(this.outputView.subarray(0, input.length));
        }

        return true;
    }

    _accumulate(frame) {
        let offset = 0;
        while (offset < frame.length) {
            const n = Math.min(frame.length - offset, this.chunkSize - this.accumulatorIndex);
            this.accumulator.set(frame.subarray(offset, offset + n), this.accumulatorIndex);
            this.accumulatorIndex += n;
            offset += n;
            if (this.accumulatorIndex === this.chunkSize) {
                this._postChunk(this.chunkSize);
                this.accumulatorIndex = 0;
            }
        }
    }

    // Transfer (not copy) the filled buffer, then start a fresh one.
    _postChunk(length) {
        const full = this.accumulator;
        this.accumulator = new Float32Array(this.chunkSize);
        const audio = length === full.length ? full : full.slice(0, length);
        this.port.postMessage({ type: 'chunk', audio: audio.buffer }, [audio.buffer]);
    }
}

registerProcessor('audio-kernel', AudioKernelProcessor);
