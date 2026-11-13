# Shader audit checkpoint

Status: Tested on Linux x86-64, g++ 11.4, CMake 3.22, Mesa lavapipe
(llvmpipe LLVM 15.0.7), 2026-11-13. Synthetic developer inputs only.

## Measured checks

- Fresh Release configure/build with `-Werror`: passed, no warnings.
- 467 unit checks, 5 CTest suites: passed. Optional SPIR-V validator was
  configured; translated modules validated for Vulkan 1.0.
- Fresh Debug build with AddressSanitizer + UndefinedBehaviorSanitizer:
  unit and compact-fixture binaries passed with no sanitizer diagnostics.
  `ASAN_OPTIONS=detect_leaks=0` was set: leaks were not checked. Sanitized
  CTest package/process workflows were not part of that claim.
- Compact fixtures: literal store, runtime addition with distinct output,
  invalid guarded runtime load. All exact buffer readbacks passed on
  software Vulkan. No physical GPU or commercial shader was tested.
- Deterministic mutation smoke: 4096 cases, 3836 structurally decoded,
  4096 rejected by the restricted translator, zero invariant failures.
  Mutated accepted programs are not executed. This is a smoke test, not
  exhaustive fuzzing or proof that arbitrary guest shaders are safe.

## Corrections made

- Shared scalar literal: both source selectors consume the same literal
  word once, rather than advancing twice. Added successful decode/PC and
  truncation regression checks. Behavior checked against AnyPS5's shared
  literal application in RdnaInstructionDecoder.cpp (GPL-2.0 attribution
  remains in shader/rdna.cpp).
- Release warning cleanup: explicit initialized branch-merge state avoids
  GCC optional maybe-uninitialized diagnostic. Removed unused demo argv
  name and completed a test record initializer. No warning suppression.

## Reproduce

```sh
cmake -S . -B build-clean -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-Werror
cmake --build build-clean -j4
# Optional validator must be installed or supplied by the developer:
PNR_SPIRV_VAL=/path/to/spirv-val ctest --test-dir build-clean --output-on-failure
./build-clean/pnr_shader_checks
cmake -S . -B build-audit -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-audit -j4
ASAN_OPTIONS=detect_leaks=0 PNR_SPIRV_VAL=/path/to/spirv-val ./build-audit/pnr_tests
ASAN_OPTIONS=detect_leaks=0 ./build-audit/pnr_shader_checks
```

Missing Vulkan/validator is printed as a skip on another host, never
relabelled as a local pass. Fixture errors include names and translator PC
messages. Low-level SPIR-V emitter and mutable public result structs remain
developer APIs, not an untrusted-code boundary. Application-level path
routing is not process sandboxing. Continue using trusted developer demos.

No benchmarks, performance promises, general PS5 compatibility or complete
shader compiler are implied by this checkpoint.
