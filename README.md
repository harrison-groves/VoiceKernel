VoiceKernel is a low-latency microphone module for the browser with built-in neural noise suppression (RNNoise). Audio runs through a C++ kernel compiled to WebAssembly inside an AudioWorklet, on the real-time audio thread. No Emscripten JS glue, Web Workers or SharedArrayBuffer are needed, so it works without cross-origin isolation (COOP/COEP) headers.

-- Installation --

Copy these three files into your project, side by side:

    voice-kernel.js
    worklet-processor.js
    processor.wasm        (~1.4 MB, mostly the RNNoise model)

Due to browser security policies, VoiceKernel requires an HTTPS connection (or localhost) and a user interaction (like a click) to initialize the AudioContext.

-- Basic Usage --

    import VoiceKernel from './voice-kernel.js';

    const mic = await VoiceKernel.create({
        mode: 'accumulate',     // 'passthrough', 'accumulate', or 'both'
        denoise: true,          // RNNoise noise suppression (adds 20 ms latency)
        gain: 1.0,              // input gain multiplier
        chunkDuration: 250,     // callback interval in milliseconds
        monitor: false,         // play processed audio to speakers (use headphones!)
        onChunk: (samples) => {
            // samples is a Float32Array: feed to STT, upload, etc.
        }
    });

    // processed (denoised) audio as a standard MediaStream
    peerConnection.addTrack(mic.stream.getAudioTracks()[0], mic.stream);

    console.log(`processing latency: ${mic.latencyMs.toFixed(2)} ms`);
    console.log(`speaking? ${mic.voiceProbability > 0.5}`);

-- API Reference --

Static Methods

    VoiceKernel.create(options) // Starts the mic and resolves once the WASM kernel is running.
                                // Rejects if mic access is denied or the kernel fails to load.

    options.mode: 'passthrough' | 'accumulate' | 'both'. Default 'passthrough'.
    options.denoise: Boolean, or a number 0..1 for partial suppression. Default false.
        Any number (even 0) switches RNNoise on, so setDenoise() can change it later.
    options.gain: Float. Default 1.0.
    options.chunkDuration: Integer ms per onChunk callback. Default 250.
    options.monitor: Boolean. Play processed audio to the speakers. Default false.
    options.onChunk: Function(Float32Array). Called with each chunk in accumulate/both modes.
    options.echoCancellation / noiseSuppression / autoGainControl:
        Booleans passed to getUserMedia: the browser's own built-in processing,
        separate from VoiceKernel's `denoise`. Default false.

Instance Properties

    stream            // MediaStream of the processed audio (WebRTC, MediaRecorder, etc.)
    latencyMs         // Latency added by VoiceKernel's processing (excludes browser I/O latency)
    voiceProbability  // 0..1: how likely the latest 10 ms is speech (needs denoise on)

Instance Methods

    setDenoise(amount)            // 0 (original) to 1 (fully denoised), smoothed. Needs denoise on at create.
    setGain(value)                // Smoothed in the kernel, so no clicks. 0.0 mutes, 1.0 is unity.
    setMode(mode, chunkDuration)  // Change chunking without restarting.
    setMonitor(on)                // Toggle speaker playback.
    getAnalyser()                 // The native AnalyserNode, for waveforms or spectra.
    destroy()                     // Stop the mic and release everything.

Operational Modes

    Mode         Behavior                                         Use Case
    passthrough  Processed audio only (stream / monitor).         Calls, live monitoring, visualizers.
    accumulate   Also collects samples into chunkDuration chunks.  STT, recording, server streaming.
    both         Same as accumulate.                               Kept for compatibility.

    Gain and denoising happen inside the kernel, so `stream`, the speakers and onChunk all receive the same audio.

-- Performance --

Measured in Chrome on a desktop PC, with a synthetic voice-like test signal plus white noise:

    Latency (denoise off)    448 samples  =  9.33 ms
    Latency (denoise on)     1408 samples = 29.33 ms
    Noise-only sections      -30.8 dB -> -81.9 dB
    Voice level              unchanged (-21.5 dB -> -21.6 dB)
    CPU                      ~2.3% of one core with denoise on

Numbers for real speech will differ; a proper benchmark (PESQ / DNSMOS on the
DNS Challenge test set) is on the to-do list.

-- Architecture --

    mic → AudioWorklet (128-sample blocks)
            → C++ kernel: buffers into 480-sample frames (10 ms @ 48 kHz)
              → processFrame()   ← RNNoise + wet/dry mix (dry delayed to match)
              → gain + fade-in
          → stream / analyser / speakers / onChunk

The frame buffer adds 448 samples (9.33 ms), the minimum that never underruns
when adapting 128-sample blocks to 480-sample frames. RNNoise adds two frames
(960 samples, 20 ms): each call outputs the previous frame, overlap-added.

    Sample Rate: 48 kHz.
    Buffer Format: 32-bit float (Float32Array).

-- Building --

Requires Emscripten (emsdk). On Windows:

    .\build.ps1

On macOS / Linux:

    make

Either produces processor.wasm from src/processor.cpp and the vendored RNNoise
source in third_party/rnnoise.

-- Credits --

Noise suppression is RNNoise v0.2 by Jean-Marc Valin / Xiph.Org Foundation,
BSD-3-Clause. See third_party/rnnoise/COPYING.
