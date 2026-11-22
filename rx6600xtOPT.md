# RX 6600 XT FP16 optimization notes

What we changed to take the benchmark from ~4.9 TFLOPS to ~19.4 TFLOPS on the
RX 6600 XT in this machine, and why the number stopped where it did.

Measured on: AMD Radeon RX 6600 XT (RADV NAVI23), Mesa 25.2.8, Ubuntu 24.04.
`timestampPeriod = 10 ns`.

## Result

| Shader | Config | GPU time | FP16 TFLOPS |
|---|---|---|---|
| `fp16.comp` (baseline) | scalar `float16_t`, 8 accum, 256 threads | ~14.1 ms | ~4.86 |
| `fp16_packed.comp` (optimized) | `f16vec2`, 32 accum, unroll 8, 512 threads | ~56.8 ms | ~19.35 |

The optimized version is about **4x** the baseline and lands essentially on the
card's game-clock FP16 peak (see "Why 19.4 is the ceiling" below).

Run-to-run spread observed: baseline 4.72–5.04, packed 18.96–19.39. Best packed
seen during tuning was 19.39.

## Hardware reference (RX 6600 XT, Navi 23)

- 32 CUs, 2048 shading units
- Base clock 1968 MHz, Game clock 2359 MHz, Boost clock 2589 MHz
- Theoretical FP32: 10.60 TFLOPS at boost
- Theoretical FP16 (2:1 packed): 21.21 TFLOPS at boost

Useful derived numbers (same 2048 x 2 FLOP/cycle x clock math):

| Clock | FP32 peak | Packed FP16 peak |
|---|---|---|
| 1968 MHz base | 8.06 TFLOPS | 16.12 TFLOPS |
| 2359 MHz game | 9.66 TFLOPS | 19.33 TFLOPS |
| 2589 MHz boost | 10.60 TFLOPS | 21.21 TFLOPS |

## Why the baseline stopped at ~4.9 TFLOPS

The baseline shader uses scalar `float16_t`:

```glsl
float16_t a = ...;
a = fma(a, multiplier, addition);
```

On RDNA2 a scalar half FMA executes at the same rate as an FP32 FMA. It does not
use the second FP16 lane, so the hard ceiling is the FP32 rate (~9.7 TFLOPS at
game clock), not the 2:1 FP16 rate. On top of that the loop does 8 FMAs plus
counter/compare/branch overhead, so ~50% of the FP32 rate is expected.

Measured 4.86 TFLOPS ≈ 50% of the 9.66 TFLOPS game-clock FP32 peak, and only
~25% of the packed FP16 peak. The instruction was the limit, not the GPU.

ISA check confirmed it: the baseline emits **0** packed half2 FMAs.

## What we changed

1. **Packed half2 arithmetic.** Accumulators are `f16vec2` instead of
   `float16_t`. The compiler emits `v_pk_fma_f16`, which does two FP16 FMAs per
   instruction. This is what unlocks the 2:1 rate.
2. **32 independent accumulators.** 32 × `f16vec2` = 64 FP16 lanes in flight,
   enough ILP to hide FMA latency without exhausting registers.
3. **Unroll the loop by 8.** 8 × 32 = 256 packed FMAs per loop body, so the
   counter/compare/branch is amortized to well under 1%.
4. **`local_size_x = 512`.** Best occupancy/register balance on this GPU. (256
   was close; 128 was clearly worse.)

Core of `fp16_packed.comp`:

```glsl
layout(local_size_x = 512) in;
...
f16vec2 acc0 = f16vec2(0.1, 0.2);
...  // acc0 .. acc31
f16vec2 multiplier = f16vec2(0.999);
f16vec2 addition = f16vec2(0.001);

for (uint iter = 0; iter < params.iterations; iter += 8)
{
    acc0 = fma(acc0, multiplier, addition);
    ...  // 32 accumulators
    // repeated 8 times total = 256 packed FMAs
}
```

FLOP accounting for the benchmark: 32 `f16vec2` × 2 lanes = **64 scalar FMAs
per thread per iteration**, i.e. pass `64` as `fmasPerIteration`.

## Tuning sweep (best of 4 runs, RX 6600 XT)

| Variant | FP16 TFLOPS |
|---|---|
| scalar, 8 accum, 256 threads | 5.04 |
| packed, 8 accum, no unroll, 256 | 10.94 |
| packed, 8 accum, unroll 4, 256 | 11.92 |
| packed, 8 accum, unroll 8, 256 | 12.15 |
| packed, 16 accum, unroll 4, 256 | 15.15 |
| packed, 32 accum, unroll 4, 256 | 17.94 |
| packed, 32 accum, unroll 4, 512 | 19.17 |
| packed, 24 accum, unroll 8, 512 | 19.25 |
| **packed, 32 accum, unroll 8, 512** | **19.39** |

Takeaways: packing roughly doubled throughput immediately; then more
independent accumulators mattered more than unrolling; 512 threads beat 256;
24 vs 32 accumulators was within noise. Diminishing returns after ~16.

### Measurement trap to remember

Early in the sweep, 64- and 128-thread runs looked like 33 and 18.7 TFLOPS. Those
were **wrong**: `fp16_benchmark.cpp` originally hardcoded `WORKGROUP_SIZE = 256`,
so it over-counted invocations (and therefore FLOPs) when the shader used fewer
threads. The benchmark now takes the local size as an argument so the FLOP count
matches the shader. Always make sure `local_size_x` in the shader equals the
local size passed to the benchmark.

## ISA proof

`RADV_DEBUG=asm` dumps the compiled shader. Count packed FMAs:

```bash
# baseline: 0
RADV_DEBUG=asm ./fp16_benchmark 0 fp16.comp.spv 8 256 2>&1 | grep -c v_pk_fma_f16

# optimized: 256 (32 accumulators x unroll 8)
RADV_DEBUG=asm ./fp16_benchmark 0 fp16_packed.comp.spv 64 512 2>&1 | grep -c v_pk_fma_f16
```

If the optimized shader ever reports 0, the driver did not pack the `f16vec2`
and the performance will fall back toward the baseline.

## Why 19.4 is the ceiling (don't chase 21.2)

The 21.21 TFLOPS figure assumes the full 2589 MHz boost clock. A sustained ALU
loop does not hold boost. At the **game clock of 2359 MHz**, the packed FP16 peak
is:

```
2048 units x 2 FLOP x 2 (packed) x 2.359 GHz = 19.33 TFLOPS
```

Measured 19.35 TFLOPS is therefore ~100% of the game-clock peak. The gap to
21.21 is clock, not code. Squeezing more would mean raising clocks/power limits,
not optimizing the shader.

The baseline, for comparison, at 4.86 TFLOPS is ~50% of the game-clock FP32
peak, which is normal for a loop with a scalar instruction and loop overhead.

## Reproduce

```bash
glslc fp16.comp -o fp16.comp.spv
glslc fp16_packed.comp -o fp16_packed.comp.spv

g++ -O2 -std=c++17 fp16_benchmark.cpp -lvulkan -o fp16_benchmark

# baseline
./fp16_benchmark 0 fp16.comp.spv 8 256

# optimized
./fp16_benchmark 0 fp16_packed.comp.spv 64 512
```

Argument order: `[gpuIndex] [shader.spv] [fmasPerIteration] [localSize]`.

## Display stutter

The optimized run is a single dispatch that keeps the GPU busy for ~57 ms
(2,097,152 threads x 4096 iterations x 32 packed FMAs). During that window the
compositor cannot get GPU time, so the desktop visibly hitches. This is expected
for a saturating compute benchmark, not a bug.

Knobs if you want a shorter run (`fp16_benchmark.cpp`):

```cpp
constexpr uint32_t WORKGROUP_COUNT = 4096;  // total threads = this x localSize
constexpr uint32_t ITERATIONS = 4096;       // work per thread
```

Runtime scales roughly linearly with either. For example `WORKGROUP_COUNT = 512`
cuts the packed run to roughly 7 ms, which is much gentler on the display. The
baseline (~14 ms) is already fairly light.

Don't run the packed benchmark while gaming, recording, or on a call.

## Files touched

- `fp16_packed.comp` — new optimized shader (32 `f16vec2`, unroll 8, 512 threads)
- `fp16_benchmark.cpp` — optional `[shader] [fmasPerIteration] [localSize]`
  arguments; defaults still match the baseline shader
- `.gitignore` — ignore all `*.spv`
- `README.md` — build/run steps for both shaders
