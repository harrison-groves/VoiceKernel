VoiceKernel is a low-latency microphone module for the browser. Audio runs through a C++ kernel compiled to WebAssembly inside an AudioWorklet, on the real-time audio thread. No Emscripten JS glue, Web Workers or SharedArrayBuffer are needed, so it works without cross-origin isolation (COOP/COEP) headers.

-- Installation --

Copy these three files into your project, side by side:

    voice-kernel.js
    worklet-processor.js
    processor.wasm

Due to browser security policies, VoiceKernel requires an HTTPS connection (or localhost) and a user interaction (like a click) to initialize the AudioContext.

-- Basic Usage --

    import VoiceKernel from './voice-kernel.js';

    const mic = await VoiceKernel.create({
        mode: 'accumulate',     // 'passthrough', 'accumulate', or 'both'
        gain: 1.0,              // input gain multiplier
        chunkDuration: 250,     // callback interval in milliseconds
        monitor: false,         // play processed audio to speakers (use headphones!)
        onChunk: (samples) => {
            // samples is a Float32Array: feed to STT, upload, etc.
        }
    });

    // processed audio as a standard MediaStream
    peerConnection.addTrack(mic.stream.getAudioTracks()[0], mic.stream);

    console.log(`processing latency: ${mic.latencyMs.toFixed(2)} ms`);

-- API Reference --

Static Methods

    VoiceKernel.create(options) // Starts the mic and resolves once the WASM kernel is running.
                                // Rejects if mic access is denied or the kernel fails to load.

    options.mode: 'passthrough' | 'accumulate' | 'both'. Default 'passthrough'.
    options.gain: Float. Default 1.0.
    options.chunkDuration: Integer ms per onChunk callback. Default 250.
    options.monitor: Boolean. Play processed audio to the speakers. Default false.
    options.onChunk: Function(Float32Array). Called with each chunk in accumulate/both modes.
    options.echoCancellation / noiseSuppression / autoGainControl:
        Booleans passed to getUserMedia (the browser's built-in processing). Default false.

Instance Properties

    stream      // MediaStream of the processed audio (WebRTC, MediaRecorder, etc.)
    latencyMs   // Latency added by VoiceKernel's processing (excludes browser I/O latency)

Instance Methods

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

    Gain is applied inside the kernel, so `stream`, the speakers and onChunk all receive the same audio.

-- Architecture --

    mic → AudioWorklet (128-sample blocks)
            → C++ kernel: buffers into 480-sample frames (10 ms @ 48 kHz)
              → processFrame()   ← DSP / noise suppression hook
              → gain + fade-in
          → stream / analyser / speakers / onChunk

The frame buffer adds 448 samples (9.33 ms) of latency, the minimum that never
underruns when adapting 128-sample blocks to 480-sample frames.

    Sample Rate: 48 kHz.
    Buffer Format: 32-bit float (Float32Array).

-- Building --

Requires Emscripten (emsdk):

    make

This produces processor.wasm from src/processor.cpp.
