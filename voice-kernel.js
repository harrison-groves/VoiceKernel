/**
 * VoiceKernel — low latency mic audio module
 *
 * Usage:
 *   import VoiceKernel from './voice-kernel.js'
 *
 *   const mic = await VoiceKernel.create({
 *       mode: 'passthrough',     // 'passthrough' | 'accumulate' | 'both'
 *       gain: 1.0,
 *       chunkDuration: 250,
 *       monitor: false,          // play processed mic to speakers (use headphones!)
 *       echoCancellation: false,
 *       noiseSuppression: false,
 *       autoGainControl: false,
 *       onChunk: (samples) => { ... }
 *   })
 *
 *   peer.addTrack(mic.stream.getAudioTracks()[0])   // processed MediaStream
 *   mic.setGain(0.5)
 *   mic.setMode('accumulate')
 *   mic.destroy()
 */

const WORKLET_PATH = new URL('./worklet-processor.js', import.meta.url).href;
const WASM_PATH = new URL('./processor.wasm', import.meta.url).href;

const READY_TIMEOUT_MS = 5000;

class VoiceKernel {

    // private — use VoiceKernel.create() instead
    constructor({ audioCtx, micSource, workletNode, analyser, outputNode, inputStream,
                  latencySamples, options }) {
        this._audioCtx = audioCtx;
        this._micSource = micSource;
        this._workletNode = workletNode;
        this._analyser = analyser;
        this._outputNode = outputNode;
        this._inputStream = inputStream;
        this._latencySamples = latencySamples;
        this._mode = options.mode || 'passthrough';
        this._monitor = false;
    }

    static async create(options = {}) {
        const {
            mode = 'passthrough',
            gain = 1.0,
            chunkDuration = 250,
            monitor = false,
            echoCancellation = false,
            noiseSuppression = false,
            autoGainControl = false,
            onChunk = null,
        } = options;

        // 1. mic access
        const inputStream = await navigator.mediaDevices.getUserMedia({
            audio: { echoCancellation, noiseSuppression, autoGainControl, channelCount: 1 },
        });

        let audioCtx;
        try {
            // 2. audio context
            audioCtx = new AudioContext({ latencyHint: 'interactive', sampleRate: 48000 });

            // 3. fetch WASM on main thread — worklet thread cannot use fetch()
            const res = await fetch(WASM_PATH);
            if (!res.ok) throw new Error(`VoiceKernel: failed to fetch WASM (${res.status})`);
            const wasmBytes = await res.arrayBuffer();

            // 4. load worklet module
            await audioCtx.audioWorklet.addModule(WORKLET_PATH);

            // 5. create worklet node
            const workletNode = new AudioWorkletNode(audioCtx, 'audio-kernel', {
                processorOptions: {
                    wasmBytes,
                    mode,
                    gain,
                    sampleRate: audioCtx.sampleRate,
                    chunkDuration,
                },
                channelCount: 1,
                channelCountMode: 'explicit',
                channelInterpretation: 'discrete',
                numberOfOutputs: 1,
            });

            // 6. wait until the WASM kernel is actually running (surfaces load errors)
            const { latencySamples } = await new Promise((resolve, reject) => {
                const timer = setTimeout(
                    () => reject(new Error('VoiceKernel: WASM kernel did not start')),
                    READY_TIMEOUT_MS);
                workletNode.port.onmessage = (e) => {
                    if (e.data.type === 'ready') { clearTimeout(timer); resolve(e.data); }
                    if (e.data.type === 'error') {
                        clearTimeout(timer);
                        reject(new Error(`VoiceKernel: ${e.data.message}`));
                    }
                };
            });

            workletNode.port.onmessage = (e) => {
                if (e.data.type === 'chunk' && onChunk) {
                    onChunk(new Float32Array(e.data.audio));
                }
            };

            // 7. wire audio graph
            //   mic → worklet → analyser                 (visualisation)
            //   mic → worklet → MediaStreamDestination   (mic.stream, for WebRTC / recording)
            //   mic → worklet → speakers                 (only if monitor is on)
            const micSource = audioCtx.createMediaStreamSource(inputStream);
            const analyser = audioCtx.createAnalyser();
            analyser.fftSize = 1024;
            const outputNode = audioCtx.createMediaStreamDestination();

            micSource.connect(workletNode);
            workletNode.connect(analyser);
            workletNode.connect(outputNode);

            const mic = new VoiceKernel({
                audioCtx, micSource, workletNode, analyser, outputNode, inputStream,
                latencySamples, options,
            });
            mic.setMonitor(monitor);
            return mic;

        } catch (err) {
            inputStream.getTracks().forEach(t => t.stop());
            if (audioCtx) audioCtx.close();
            throw err;
        }
    }

    /** Processed audio as a MediaStream: pass to RTCPeerConnection, MediaRecorder, etc. */
    get stream() {
        return this._outputNode ? this._outputNode.stream : null;
    }

    /** Latency added by VoiceKernel's processing, in milliseconds (excludes browser I/O). */
    get latencyMs() {
        return this._audioCtx ? (this._latencySamples / this._audioCtx.sampleRate) * 1000 : 0;
    }

    /**
     * Set the output gain. Smoothed inside the kernel, so no clicks.
     * @param {number} value - 0.0 (silent) to 2.0 (double volume), 1.0 is unity
     */
    setGain(value) {
        const v = Math.max(0, Math.min(4, Number(value) || 0));
        this._workletNode.port.postMessage({ type: 'param', id: 0, value: v });
    }

    /**
     * Switch chunking mode at runtime without restarting the pipeline.
     * @param {string} mode - 'passthrough' | 'accumulate' | 'both'
     * @param {number} chunkDuration - optional new chunk size in ms
     */
    setMode(mode, chunkDuration) {
        this._mode = mode;
        this._workletNode.port.postMessage({
            type: 'setMode',
            mode,
            sampleRate: this._audioCtx.sampleRate,
            chunkDuration: chunkDuration || undefined,
        });
    }

    /**
     * Play the processed mic through the speakers. Off by default: with speakers
     * instead of headphones this causes feedback.
     * @param {boolean} on
     */
    setMonitor(on) {
        if (on === this._monitor) return;
        if (on) this._workletNode.connect(this._audioCtx.destination);
        else {
            try { this._workletNode.disconnect(this._audioCtx.destination); } catch (_) { }
        }
        this._monitor = on;
    }

    /**
     * Expose the analyser node so devs can build their own visualisations.
     * @returns {AnalyserNode}
     */
    getAnalyser() {
        return this._analyser;
    }

    /**
     * Tear down the pipeline — stops mic, closes audio context, cleans up.
     */
    destroy() {
        if (this._micSource) { this._micSource.disconnect(); this._micSource = null; }
        if (this._workletNode) {
            this._workletNode.port.onmessage = null;
            this._workletNode.disconnect();
            this._workletNode = null;
        }
        if (this._analyser) { this._analyser.disconnect(); this._analyser = null; }
        if (this._outputNode) { this._outputNode = null; }
        if (this._audioCtx) { this._audioCtx.close(); this._audioCtx = null; }
        if (this._inputStream) {
            this._inputStream.getTracks().forEach(t => t.stop());
            this._inputStream = null;
        }
    }
}

export default VoiceKernel;
