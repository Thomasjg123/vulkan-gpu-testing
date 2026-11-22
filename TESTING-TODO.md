# Testing TODO

Metrics I want to add to the benchmark later. Not implemented yet.

| Metric | What it tells you | Status |
|---|---|---|
| FP16 scalar | Scalar half-precision performance | Done — `fp16.comp` |
| FP16 packed | Packed half-precision performance | Done — `fp16_packed.comp` |
| FP32 | Single-precision performance | Not started |
| FP64 | Double-precision performance | Not started |
| BF16 | Brain floating-point performance, if supported | Not started |
| Memory bandwidth | How quickly data can be moved | Not started |

## Notes for later

- Each new arithmetic metric needs: a shader, a feature check in the benchmark,
  and a matching `fmasPerIteration` value so the TFLOPS math is right.
- FP32: same structure as `fp16.comp`, just `float`.
- FP64: needs a `shaderFloat64` check; most consumer GPUs run it at 1/16 or
  1/32 rate, so expect a much lower number.
- BF16: only run if the device reports support; skip cleanly otherwise.
- Memory bandwidth: different shape from the FMA tests — measure buffer
  copy/transfer throughput (bytes moved / time) rather than FLOPs.
- Keep the same rule as `rx6600xtOPT.md`: make sure the local size and FLOP
  count passed to the benchmark match the shader, or the result is wrong.
