#include <emscripten.h>

// ─── Sizes ────────────────────────────────────────────────────────────────────
//
// The AudioWorklet hands us BLOCK samples per call, but speech models
// (RNNoise, DeepFilterNet, ...) work on FRAME-sized hops: 480 samples = 10 ms
// at 48 kHz. The kernel buffers input until a full frame is ready, processes
// it, and queues the result for output.
//
// To never run dry, the output queue starts with PRIME samples of silence.
// The minimum that works is FRAME - gcd(BLOCK, FRAME):
//   after n input samples (n a multiple of 128), at least n - 448 of them have
//   been through a full frame, so 448 samples of head start always cover the
//   next 128-sample read.
// That head start IS the added latency: 448 samples = 9.33 ms at 48 kHz.

constexpr int BLOCK = 128;
constexpr int FRAME = 480;
constexpr int GCD   = 32;                 // gcd(BLOCK, FRAME)
constexpr int PRIME = FRAME - GCD;        // 448
constexpr int RING  = 1024;               // power of two, >= PRIME + BLOCK (peak fill)
constexpr int RING_MASK = RING - 1;

static_assert(FRAME % GCD == 0 && BLOCK % GCD == 0, "GCD must divide both sizes");
// Before each read the queue holds at most PRIME + BLOCK samples (576).
static_assert(RING >= PRIME + BLOCK, "output ring too small");

class AudioKernel {
public:
    // parameters
    float targetGain = 1.0f;

    void reset() {
        frameFill = 0;
        for (int i = 0; i < RING; ++i) ring[i] = 0.0f;
        readPos  = 0;
        writePos = PRIME;               // PRIME samples of silence already queued
        gain     = targetGain;
        fade     = 0.0f;
        underruns = 0;
    }

    void process(const float* input, float* output, int numFrames) {
        // 1. collect input into frames; run each full frame through the DSP
        for (int i = 0; i < numFrames; ++i) {
            frameIn[frameFill++] = input[i];
            if (frameFill == FRAME) {
                processFrame(frameIn, frameOut);
                for (int j = 0; j < FRAME; ++j) {
                    ring[writePos & RING_MASK] = frameOut[j];
                    ++writePos;
                }
                frameFill = 0;
            }
        }

        // 2. emit numFrames samples, applying smoothed gain and start-up fade
        for (int i = 0; i < numFrames; ++i) {
            float s = 0.0f;
            if (writePos - readPos > 0u) {   // wrap-safe (counters wrap after ~24 h)
                s = ring[readPos & RING_MASK];
                ++readPos;
            } else {
                ++underruns;            // only possible if the block size isn't 128
            }

            gain += (targetGain - gain) * GAIN_SMOOTH;   // one-pole: no zipper noise
            if (fade < 1.0f) fade += FADE_STEP;
            if (fade > 1.0f) fade = 1.0f;

            output[i] = s * gain * fade;
        }
    }

    int latencySamples() const { return PRIME; }
    int underrunCount()  const { return underruns; }

private:
    // ── The DSP hook ──────────────────────────────────────────────────────────
    // Called once per 480-sample frame. Noise suppression goes here:
    // RNNoise first as a baseline, then your own model.
    void processFrame(const float* in, float* out) {
        for (int i = 0; i < FRAME; ++i) out[i] = in[i];
    }

    static constexpr float GAIN_SMOOTH = 0.002f;           // ~10 ms time constant
    static constexpr float FADE_STEP   = 1.0f / FRAME;     // fade in over 10 ms of output

    float frameIn[FRAME]  = {};
    float frameOut[FRAME] = {};
    int   frameFill = 0;

    float ring[RING] = {};
    unsigned readPos  = 0;          // monotonic counters; masked when indexing
    unsigned writePos = PRIME;

    float gain = 1.0f;
    float fade = 0.0f;
    int   underruns = 0;
};

// Everything is static: no malloc, no heap growth, so the module needs no
// imports and can be instantiated with a plain WebAssembly.instantiate().
// Each AudioWorkletNode instantiates its own copy of the module, so one
// kernel per module is enough.
static AudioKernel kernel;

// C++ owns these — JS asks for their addresses, never invents them
static float inputBuffer[BLOCK];
static float outputBuffer[BLOCK];

extern "C" {
    EMSCRIPTEN_KEEPALIVE
    AudioKernel* create_kernel() { kernel.reset(); return &kernel; }

    EMSCRIPTEN_KEEPALIVE float* get_input_ptr()  { return inputBuffer; }
    EMSCRIPTEN_KEEPALIVE float* get_output_ptr() { return outputBuffer; }

    EMSCRIPTEN_KEEPALIVE
    void process_audio(AudioKernel* k, float* input, float* output, int numFrames) {
        k->process(input, output, numFrames);
    }

    // id 0 = gain (expand with more ids as you add parameters)
    EMSCRIPTEN_KEEPALIVE
    void set_parameter(AudioKernel* k, int id, float value) {
        if (id == 0) k->targetGain = value;
    }

    EMSCRIPTEN_KEEPALIVE int get_latency_samples(AudioKernel* k) { return k->latencySamples(); }
    EMSCRIPTEN_KEEPALIVE int get_underrun_count(AudioKernel* k)  { return k->underrunCount(); }

    EMSCRIPTEN_KEEPALIVE
    void destroy_kernel(AudioKernel* k) { k->reset(); }
}
