# Vulkan GPU FP16 Test

Two small programs plus one compute shader:

| File | What it does |
|---|---|
| `gpu_detect.cpp` | Lists every Vulkan device the driver exposes: name, vendor, type, API version, memory heaps |
| `fp16.comp` | GLSL compute shader: 8 independent `float16_t` FMA chains per thread (baseline) |
| `fp16_packed.comp` | Optimized shader: packed `f16vec2` FMAs, 32 accumulators, unrolled. See `rx6600xtOPT.md` |
| `fp16_benchmark.cpp` | Builds a compute pipeline, runs a shader, times it on the GPU, prints TFLOPS |

There is no build system on purpose. Every step is a command you run yourself, so
when something fails you know exactly which step failed and what to look up.

**Read the "Expected result" under each step before running the next one.** If a
step does not print what it should, stop there and use the Troubleshooting
section. Do not skip ahead — a missing driver looks identical to a broken build
if you only look at the last command.

---

## What you need, conceptually

Four separate things. They fail independently, which is why the steps below
check them one at a time.

1. **A C++ compiler** — turns the `.cpp` files into programs.
2. **Vulkan headers and the loader library** (`vulkan/vulkan.h`, `libvulkan.so`) —
   lets the code compile and link.
3. **A Vulkan driver (ICD)** — the vendor/Mesa driver that actually talks to your
   GPU. The loader finds it through files in `/usr/share/vulkan/icd.d/`.
4. **A GLSL-to-SPIR-V compiler** (`glslc` or `glslangValidator`) — turns
   `fp16.comp` into `fp16.comp.spv`, which is the only thing the GPU can load.

Numbers 1 and 2 are ordinary dev packages. Number 3 is the one people get wrong,
and number 4 is easy to forget because the C++ still compiles without it.

---

## Step 0 — Find out what GPU you have

```bash
lspci -nn | grep -Ei 'vga|3d|display'
```

Expected result: at least one line naming your GPU, for example
`Advanced Micro Devices, Inc. [AMD/ATI] Navi 23 [Radeon RX 6600/6600 XT/6600M] [1002:73ff]`.

Note the vendor:
- `1002` = AMD
- `10de` = NVIDIA
- `8086` = Intel
- no GPU line at all = VM, container, or unusual hardware. See Troubleshooting.

You need this to pick the right driver package in Step 1.

---

## Step 1 — Install the compiler, headers, loader, driver, and shader compiler

Pick the block for your distro. Install everything in one command only because
the package manager reports missing package names clearly; the *verification* is
still done one piece at a time in Step 2.

### Debian / Ubuntu

```bash
sudo apt update
sudo apt install -y build-essential libvulkan-dev vulkan-tools mesa-vulkan-drivers glslc
```

- `build-essential` → `g++`
- `libvulkan-dev` → `vulkan/vulkan.h` and `libvulkan.so`
- `vulkan-tools` → `vulkaninfo` (used to verify the driver)
- `mesa-vulkan-drivers` → AMD (RADV) and Intel (ANV) drivers, plus llvmpipe
- `glslc` → shader compiler




### Optional but recommended: validation layers

These are not required to run, but they turn silent Vulkan misuse into loud
errors. Install now and use them if anything behaves strangely later.

```bash
# Debian/Ubuntu
sudo apt install -y vulkan-validationlayers

---

## Step 2 — Verify the driver is actually installed and visible

This is the most important step. Do it before building anything.

```bash
vulkaninfo --summary
```

Expected result: a `Devices:` section listing your real GPU, for example
`AMD Radeon RX 6600 XT (RADV NAVI23)`, `NVIDIA GeForce ...`, or `Intel(R) ...`.

Then check where that driver came from:

```bash
ls /usr/share/vulkan/icd.d/
```

Expected result: a JSON file matching your vendor, such as
`radeon_icd.x86_64.json`, `nvidia_icd.json`, or `intel_icd.x86_64.json`.
`lvp_icd.x86_64.json` is llvmpipe (CPU software renderer) — it is fine to have
it, but it must not be your only device.

**If `vulkaninfo` shows only `llvmpipe`**, you have no hardware driver. Stop and
go to Troubleshooting. Building and running anyway will "work" but will measure
your CPU at roughly 1% of GPU speed, which is a very confusing result.

If `vulkaninfo` is not found, Step 1 did not install `vulkan-tools`.

---

## Step 3 — Compile the shader

The C++ programs load `fp16.comp.spv` at runtime. It is not in the repo, so this
step must be done first and its output must sit next to the program you run.

```bash
glslc fp16.comp -o fp16.comp.spv
```

For the optimized shader (see `rx6600xtOPT.md`), compile it as well:

```bash
glslc fp16_packed.comp -o fp16_packed.comp.spv
```

Expected result: no output, exit code 0. Confirm the file exists and is non-empty:

```bash
ls -l fp16.comp.spv
```

A SPIR-V file is a multiple of 4 bytes. A few hundred bytes to a few KB is
normal.

If `glslc` is not found, try the older equivalent:

```bash
glslangValidator -V fp16.comp -o fp16.comp.spv
```

Common failure: an error mentioning
`GL_EXT_shader_explicit_arithmetic_types_float16`. That means your `glslc` is
too old or the shader was saved with the wrong encoding. Check the version:

```bash
glslc --version
```

---

## Step 4 — Compile the device lister

Do this on its own first. It has no shader dependency, so if it fails the
problem is the compiler, headers, or linker — not the GPU.

```bash
g++ -O2 -std=c++17 gpu_detect.cpp -lvulkan -o gpu_detect
```

Expected result: no output, exit code 0, and a `gpu_detect` file appears.

Check the exit code explicitly if you are unsure:

```bash
echo $?
```

`0` means success.

Failure meanings:
- `vulkan/vulkan.h: No such file or directory` → headers missing, re-check Step 1.
- `undefined reference to vkCreateInstance` → linker cannot find the loader;
  `libvulkan-dev` / `vulkan-loader-devel` missing.
- `cannot find -lvulkan` → same as above.

---

## Step 5 — Run the device lister

```bash
./gpu_detect
```

Expected result: a list of GPUs. A healthy discrete-GPU machine looks like:

```
Detected Vulkan device(s): 2

==========================
GPU #0
==========================
name: AMD Radeon RX 6600 XT (RADV NAVI23)
VendorAMD
Device  typeDiscrete GPU
Vendor id 0x4098
Device ID: 0x29695
Vulkan api: 1.4.318

Memory Heaps:
Heap0 : 62.86GB
Heap1 : 7.98GB (device local)
```

Notes on what you are seeing:
- A heap marked `(device local)` is VRAM. That is the one you care about.
- `VendorAMD` and `Device  typeDiscrete GPU` are missing a space — cosmetic bug
  in the source, not a problem with your machine.
- The `0x` vendor/device IDs are printed in **decimal**, so `0x4098` is actually
  `0x1002` (AMD). Cosmetic bug; ignore the hex-looking prefix.
- `unknown???` as vendor usually means Mesa's software renderer
  (`vendorID 0x10005`). See Troubleshooting.
- If you only see `llvmpipe`, stop. You are not using the GPU.

---

## Step 6 — Compile the benchmark

```bash
g++ -O2 -std=c++17 fp16_benchmark.cpp -lvulkan -o fp16_benchmark
```

Expected result: no output, exit code 0.

Same failure meanings as Step 4.

---

## Step 7 — Run the benchmark on the GPU you want

The program takes the GPU index as an argument. Use the index from `./gpu_detect`.

```bash
./fp16_benchmark 0
```

Expected result: the selected GPU's name, `FP16 arithmetic: Supported`, and a
TFLOPS number:

```
Selected GPU #0
Name: AMD Radeon RX 6600 XT (RADV NAVI23)
FP16 arithmetic: Supported

========== FP16 BENCHMARK ==========
GPU: AMD Radeon RX 6600 XT (RADV NAVI23)
Invocations: 1048576
Iterations: 4096
Elapsed GPU time: 14.154 ms
Total operations: 68719476736
FP16 performance: 4.855 TFLOPS
====================================
```

### Optional arguments

`fp16_benchmark` accepts four positional arguments, all optional:

```
./fp16_benchmark [gpuIndex] [shader.spv] [fmasPerIteration] [localSize]
```

Defaults are `3`, `fp16.comp.spv`, `8`, `256`, which match the baseline shader.
To run the optimized packed shader:

```bash
./fp16_benchmark 0 fp16_packed.comp.spv 64 512
```

`fmasPerIteration` must match what the shader does, otherwise the reported
TFLOPS is wrong: 8 for `fp16.comp` (8 scalar FMAs), 64 for `fp16_packed.comp`
(32 `f16vec2` × 2 lanes). `localSize` must match `local_size_x` in the shader
(256 and 512 respectively).

**The argument is required in practice.** The source defaults to GPU index `3`,
which does not exist on most machines. If you run `./fp16_benchmark` with no
argument and see `GPU index does not exist`, that is this bug, not your setup.
Always pass the index explicitly.

**Run it from the directory that contains `fp16.comp.spv`.** The program opens
`fp16.comp.spv` by relative path. Running it from elsewhere gives
`Could not open fp16.comp.spv` even though Step 3 succeeded.

Sanity checks on the number:
- Software renderer (llvmpipe): roughly `0.01–0.1 TFLOPS`.
- Real discrete GPU: several TFLOPS. A 2–3 order-of-magnitude gap from the CPU
  entry is the signal that you really hit the GPU.
- This shader uses scalar `float16_t`, not packed `f16vec2`, so on many GPUs it
  runs at the same rate as FP32. It measures half-precision *instruction*
  throughput, not the 2× packed rate some datasheets quote.

---

## Troubleshooting

Find your symptom and check that one thing.

**`vulkaninfo` shows only llvmpipe / `unknown???` vendor in `gpu_detect`**
Your hardware driver is missing. Check the ICD directory and install the right
package: AMD/Intel → `mesa-vulkan-drivers`; NVIDIA → proprietary driver. Then
re-run Step 2. Do not trust benchmark numbers until a real GPU appears.

**`vkCreateInstance` fails or `vulkaninfo` errors with a loader problem**
The loader exists but found no usable ICD. Run with debug output to see what it
tries:

```bash
VK_LOADER_DEBUG=all ./gpu_detect
```

**`glslc: command not found`**
Package is `glslc` on Debian/Ubuntu, `glslc` from `shaderc` on Fedora/Arch.
Alternatively use `glslangValidator` (Step 3).

**`Could not open fp16.comp.spv`**
Either Step 3 was skipped, or you are running from a different directory than the
one containing the `.spv`. Check with `ls fp16.comp.spv`.

**`GPU index does not exist`**
You passed an index that `./gpu_detect` did not list, or you passed none and hit
the default of `3`. Use an index from Step 5.

**`This GPU does not support shader FP16`**
The device is real but lacks `shaderFloat16`. Try another index (e.g. the
discrete GPU instead of the integrated one). Software renderers usually report
support, so this is more likely an old or limited driver.

**`No timestamp-capable compute queue found`**
Rare, but some drivers do not expose timestamps on compute queues. There is no
workaround in this code; it needs GPU timestamps to measure anything.

**Permission denied opening the GPU device**
Add your user to the `video` and `render` groups, then log out and back in:

```bash
sudo usermod -aG video,render "$USER"
```

**NVIDIA: `vulkaninfo` works but `gpu_detect` does not see the card**
Confirm `/usr/share/vulkan/icd.d/nvidia_icd.json` exists. If you are on a laptop
with hybrid graphics, the NVIDIA GPU may only be exposed on demand; use
`__NV_PRIME_RENDER_OFFLOAD=1 ./fp16_benchmark 0` or check `vulkaninfo` for both
devices.

**WSL2**
The loader is under `/usr/lib/wsl/lib` and the ICD is provided by the Windows
driver. If nothing is found, point the loader at the WSL ICD:

```bash
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/dzn_icd.x86_64.json ./gpu_detect
```

Also ensure the Windows-side GPU driver supports WSL (recent AMD/NVIDIA/Intel
drivers do).

**No X/Wayland session (SSH, server, container)**
This code creates no window and no surface, so it runs fine headless. If it fails
only over SSH, the issue is device permissions (see above), not the display.

**Validation layers**
To get loud errors instead of silent misbehaviour, run with them enabled after
installing from Step 1:

```bash
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation ./fp16_benchmark 0
```

The program does not enable layers itself; this environment variable forces the
loader to.

---

## Quick reference (the whole happy path)

Only use this once you have done the steps above at least once and know each one
passes.

```bash
# 1. driver check
vulkaninfo --summary

# 2. shaders
glslc fp16.comp -o fp16.comp.spv
glslc fp16_packed.comp -o fp16_packed.comp.spv

# 3. programs
g++ -O2 -std=c++17 gpu_detect.cpp -lvulkan -o gpu_detect
g++ -O2 -std=c++17 fp16_benchmark.cpp -lvulkan -o fp16_benchmark

# 4. list devices, then benchmark the one you want
./gpu_detect
./fp16_benchmark 0 fp16.comp.spv 8 256          # baseline (~5 TFLOPS)
./fp16_benchmark 0 fp16_packed.comp.spv 64 512  # optimized (~19 TFLOPS)
```
