CC  = emcc
OUT = processor.wasm

RNNOISE     = third_party/rnnoise
RNNOISE_SRC = $(RNNOISE)/src/denoise.c $(RNNOISE)/src/rnn.c $(RNNOISE)/src/pitch.c \
              $(RNNOISE)/src/kiss_fft.c $(RNNOISE)/src/celt_lpc.c $(RNNOISE)/src/nnet.c \
              $(RNNOISE)/src/nnet_default.c $(RNNOISE)/src/parse_lpcnet_weights.c \
              $(RNNOISE)/src/rnnoise_data.c $(RNNOISE)/src/rnnoise_tables.c
SRC = src/processor.cpp $(RNNOISE_SRC)

# Standalone .wasm (no Emscripten JS glue): the worklet instantiates it directly.
# No ALLOW_MEMORY_GROWTH: growth would add an import the worklet must supply.
# -msimd128 lets the compiler auto-vectorise RNNoise's plain-C path; src/compat
# supplies the os_support.h header that path needs (missing from the release).
# STACK_SIZE 1 MB: RNNoise keeps large FFT/pitch buffers on the stack and
# overflows the 64 KB default, silently corrupting memory.
EXPORTS = ["_create_kernel","_get_input_ptr","_get_output_ptr","_process_audio","_set_parameter","_get_latency_samples","_get_underrun_count","_get_vad","_is_ready","_destroy_kernel"]

FLAGS = -O3 -msimd128 -fno-exceptions -s STACK_SIZE=1048576 \
        -I$(RNNOISE)/include -I$(RNNOISE)/src -Isrc/compat \
        -s EXPORTED_FUNCTIONS='$(EXPORTS)' \
        --no-entry

all: $(OUT)

$(OUT): $(SRC)
	$(CC) $(SRC) -o $(OUT) $(FLAGS)
	@echo "Build complete: $(OUT)"

clean:
	rm -f $(OUT)

.PHONY: all clean
