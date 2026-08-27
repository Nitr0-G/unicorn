# x86-64 TLB benchmark

This public-API benchmark provides six modes:

- `dense`: sequential 64-bit loads through a 64 KiB ring.
- `hook-global`: the dense workload with a global `UC_HOOK_MEM_READ`.
- `hook-bounded`: the dense workload with a read hook on the first 4 KiB.
- `vtlb-dense`: the dense workload with identity-mapped `UC_TLB_VIRTUAL`.
- `vtlb-conflict`: loads from 16 identity-mapped pages spaced 1 MiB apart.
- `vtlb-conflict-cold`: the same conflict pattern with a fresh engine for
  every sample, including TLB growth cost.

Each timed repeat starts after a TLB flush. Warmups populate the translation
cache, and the summary reports median/min/max time, throughput, hook calls, and
TLB fill counts. Setup and TLB flush time are excluded.

## Build

First build Unicorn as a shared library with CMake. Then configure this
standalone benchmark against that build; the repository root CMake files do
not need changes.

Windows PowerShell, using an existing MSVC or Ninja build:

```powershell
cmake -S .\tests\benchmarks\tlb -B .\tests\benchmarks\tlb\build `
  -DUNICORN_BUILD_DIR="$PWD\build_qemu72_tests" `
  -DCMAKE_BUILD_TYPE=Release
cmake --build .\tests\benchmarks\tlb\build --config Release
.\tests\benchmarks\tlb\build\bin\tlb_bench.exe all
```

Linux, from the repository root:

```sh
cmake -S tests/benchmarks/tlb -B tests/benchmarks/tlb/build \
  -DUNICORN_BUILD_DIR="$PWD/build" -DCMAKE_BUILD_TYPE=Release
cmake --build tests/benchmarks/tlb/build -j
LD_LIBRARY_PATH="$PWD/build${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  tests/benchmarks/tlb/build/bin/tlb_bench all
```

## Run

Select one mode or `all`, then optionally set warmups, timed repeats, and guest
load count:

```text
tlb_bench [all|dense|hook-global|hook-bounded|vtlb-dense|vtlb-conflict|
           vtlb-conflict-cold]
          [--warmup N] [--repeats N] [--loads N]
```

Dense modes require `--loads` to be a multiple of eight. Defaults are two
warmups, seven repeats, and 1,048,576 loads.
