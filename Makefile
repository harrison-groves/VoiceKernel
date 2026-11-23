CC  = emcc
SRC = src/processor.cpp
OUT = processor.wasm

# Standalone .wasm (no Emscripten JS glue): the worklet instantiates it directly.
# No ALLOW_MEMORY_GROWTH: the kernel uses only static memory, and growth would
# add an `env.emscripten_notify_memory_growth` import the worklet must supply.
FLAGS = -O3 \
        -std=c++17 \
        -fno-exceptions \
        -s EXPORTED_FUNCTIONS='["_create_kernel","_get_input_ptr","_get_output_ptr","_process_audio","_set_parameter","_get_latency_samples","_get_underrun_count","_destroy_kernel"]' \
        --no-entry

all: $(OUT)

$(OUT): $(SRC)
	$(CC) $(SRC) -o $(OUT) $(FLAGS)
	@echo "Build complete: $(OUT)"

clean:
	rm -f $(OUT)

.PHONY: all clean
