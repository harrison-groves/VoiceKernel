#include <emscripten.h>
#include "rnnoise.h"

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

// RNNoise output lags its input by two frames: each call synthesises the
// previous frame's spectrum (kept one frame back for its pitch filter), and
// that spectrum's window is overlap-added with the frame before it.
// The dry signal is delayed by the same amount so wet/dry mixing stays aligned.
constexpr int NS_DELAY = 2 * FRAME;       // 960 samples = 20 ms
constexpr int DRY_RING = 1024;            // power of two, > NS_DELAY
constexpr float PCM_SCALE = 32768.0f;     // RNNoise expects 16-bit sample range

static_assert(FRAME % GCD == 0 && BLOCK % GCD == 0, "GCD must divide both sizes");
// Before each read the queue holds at most PRIME + BLOCK samples (576).
static_assert(RING >= PRIME + BLOCK, "output ring too small");

class AudioKernel {
public:
    // parameters
    float targetGain = 1.0f;
    float targetMix  = 0.0f;   // 0 = dry, 1 = fully denoised
    bool  nsEnabled  = false;  // fixed before processing starts (it changes latency)

    void reset() {
        if (!ns) ns = rnnoise_create(nullptr);   // built-in model weights
        else     rnnoise_init(ns, nullptr);
        for (int i = 0; i < DRY_RING; ++i) dryRing[i] = 0.0f;
        dryPos = 0;
        mix = targetMix;
        vad = 0.0f;

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

    int   latencySamples() const { return PRIME + (nsEnabled ? NS_DELAY : 0); }
    int   underrunCount()  const { return underruns; }
    float voiceProbability() const { return vad; }
    bool  ready() const { return ns != nullptr; }

private:
    // ── The DSP hook ──────────────────────────────────────────────────────────
    // Called once per 480-sample frame. RNNoise is the baseline denoiser;
    // swap in your own model here later and compare.
    void processFrame(const float* in, float* out) {
        if (!nsEnabled || !ns) {
            for (int i = 0; i < FRAME; ++i) out[i] = in[i];
            return;
        }

        for (int i = 0; i < FRAME; ++i) nsIn[i] = in[i] * PCM_SCALE;
        vad = rnnoise_process_frame(ns, nsOut, nsIn);

        for (int i = 0; i < FRAME; ++i) {
            // dry path delayed to line up with RNNoise's output
            dryRing[(dryPos + NS_DELAY) & (DRY_RING - 1)] = in[i];
            float dry = dryRing[dryPos & (DRY_RING - 1)];
            ++dryPos;

            mix += (targetMix - mix) * GAIN_SMOOTH;      // smooth A/B switching
            float wet = nsOut[i] / PCM_SCALE;
            out[i] = dry + (wet - dry) * mix;
        }
    }

    static constexpr float GAIN_SMOOTH = 0.002f;           // ~10 ms time constant
    static constexpr float FADE_STEP   = 1.0f / FRAME;     // fade in over 10 ms of output

    DenoiseState* ns = nullptr;
    float nsIn[FRAME]  = {};
    float nsOut[FRAME] = {};
    float dryRing[DRY_RING] = {};
    unsigned dryPos = 0;
    float mix = 0.0f;
    float vad = 0.0f;

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

// The kernel is static; only RNNoise mallocs (once, at create). The heap is
// fixed-size (no growth), so the module needs no imports and can be
// instantiated with a plain WebAssembly.instantiate().
// Each AudioWorkletNode instantiates its own copy of the module, so one
// kernel per module is enough.
// Each sample is written NS_DELAY slots ahead of the read position.
static_assert(DRY_RING > NS_DELAY, "dry ring too small");
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

    // id 0 = gain
    // id 1 = denoise amount, 0..1 (wet/dry mix)
    // id 2 = denoise enabled, 0/1 (set once before processing: changes latency)
    EMSCRIPTEN_KEEPALIVE
    void set_parameter(AudioKernel* k, int id, float value) {
        if (id == 0) k->targetGain = value;
        if (id == 1) k->targetMix  = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
        if (id == 2) k->nsEnabled  = value != 0.0f;
    }

    EMSCRIPTEN_KEEPALIVE int   get_latency_samples(AudioKernel* k) { return k->latencySamples(); }
    EMSCRIPTEN_KEEPALIVE int   get_underrun_count(AudioKernel* k)  { return k->underrunCount(); }
    EMSCRIPTEN_KEEPALIVE float get_vad(AudioKernel* k)             { return k->voiceProbability(); }
    EMSCRIPTEN_KEEPALIVE int   is_ready(AudioKernel* k)            { return k->ready() ? 1 : 0; }

    EMSCRIPTEN_KEEPALIVE
    void destroy_kernel(AudioKernel* k) { k->reset(); }
}
