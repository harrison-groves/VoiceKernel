# Windows build without make. Same flags as the Makefile.
# Usage (from the VoiceKernel folder):  .\build.ps1
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$rn = 'third_party/rnnoise'
$src = @('src/processor.cpp') + (@(
    'denoise', 'rnn', 'pitch', 'kiss_fft', 'celt_lpc', 'nnet',
    'nnet_default', 'parse_lpcnet_weights', 'rnnoise_data', 'rnnoise_tables'
) | ForEach-Object { "$rn/src/$_.c" })

$exports = "EXPORTED_FUNCTIONS=['_create_kernel','_get_input_ptr','_get_output_ptr','_process_audio','_set_parameter','_get_latency_samples','_get_underrun_count','_get_vad','_is_ready','_destroy_kernel']"

emcc @src -o processor.wasm -O3 -msimd128 -fno-exceptions -s STACK_SIZE=1048576 `
    "-I$rn/include" "-I$rn/src" "-Isrc/compat" -s $exports --no-entry
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host ("Build complete: processor.wasm ({0:N0} KB)" -f ((Get-Item processor.wasm).Length / 1KB))
