# PNR Risk Register

Version 0.1 · Phase 0 deliverable · 2026-11-13
Severity: Critical/High/Medium/Low. Likelihood: Certain/Likely/Possible/Unlikely.

## Technical risks

| ID | Risk | Sev | Likelihood | Mitigation | Status |
|----|------|-----|-----------|------------|--------|
| T1 | Foreign-ABI binaries cannot be loaded by the host OS loader; in-process mapping, TLS, relocations and exception interop turn out to be a multi-year effort | Critical | Likely | Keep loader modular (Parser / NativeLoader / FormatAdapter); prove with controlled binaries first; treat foreign-ABI as Phase 7 research with written exit criteria | Open |
| T2 | Console GPU/shader translation to Vulkan has large correctness gaps (custom AMD IP, different memory model) | Critical | Likely | Developer-shader frontend first; capability-based device checks; per-feature validation; never pass unknown shader blobs to drivers | Open |
| T3 | Low-end devices physically cannot meet frame/memory targets | High | Certain | Honest capability reporting; profiles that degrade gracefully; explicit "cannot meet target" states | Accepted, messaging built-in |
| T4 | ABI assumptions baked into early compat services block later console-specific semantics | Medium | Possible | Versioned interfaces (see interface-contracts.md); compat functions documented and unit-tested; mocks labeled as mocks | Mitigated |
| T5 | Vulkan driver differences across NVIDIA/AMD/Intel break rendering on some GPUs | High | Likely | Validation layers in CI on multiple vendors (needs hardware runners); capability-based, not vendor-name-based, feature checks | Open (needs hardware) |
| T6 | Adaptive controller oscillation (resolution thrash) | Medium | Possible | Hysteresis + stabilization intervals; deterministic rules before any ML tuning; revert-on-regression | Designed, unimplemented |
| T7 | Shader compilation stutter ruins first-run experience | Medium | Likely | Persistent pipeline cache with driver-aware keys; async preparation; telemetry | Planned |
| T8 | In-process foreign code execution compromises launcher stability/security | High | Possible | Game code in a separate process (implemented); further sandboxing (seccomp/job objects) planned; remaining risks documented openly | Partially mitigated |

## Legal / licensing risks

| ID | Risk | Sev | Likelihood | Mitigation | Status |
|----|------|-----|-----------|------------|--------|
| L1 | DMCA 1201 / equivalent anti-circumvention exposure if protected formats are handled | Critical | Certain (if attempted) | Hard rule: no DRM bypass, no key extraction, no decryption. Protected formats produce an informative Unsupported error | Enforced |
| L2 | Copyright exposure from distributing firmware/SDK/game files | Critical | Certain (if attempted) | Repo policy: none of these files, ever; license audit in CI planned | Enforced |
| L3 | PSN terms prohibit third-party account/library/entitlement integrations | High | Likely | AccountProvider is an interface with an explicit Unsupported state until an approved integration exists | Accepted |
| L4 | User belief that buying a PS5 game grants PC execution rights | High | Certain | Ownership/distribution/execution rights modeled separately; UI explains the distinction | Designed |
| L5 | Trademark/branding exposure ("PlayStation", "PS5") in a consumer product | Medium | Likely | PNR branding; names used only to describe compatibility targets; legal review before any public release | Open |

## Product / schedule risks

| ID | Risk | Sev | Likelihood | Mitigation | Status |
|----|------|-----|-----------|------------|--------|
| P1 | Commercial PS5 compatibility is never achieved; project value collapses to the dev-runtime + launcher | High | Possible | Roadmap treats PS5 support as research (Phase 7), not a promise; every milestone ships real, independently useful software | Accepted |
| P2 | Single-maintainer bandwidth vs an 11-module scope | High | Certain | Phase gating; smallest useful increments; test-before-advance rule | Accepted |
| P3 | Benchmark envy: pressure to show numbers leads to fabricated or cherry-picked results | Medium | Possible | Rule 14 enforced: methodology doc, recorded baselines, "no benchmarks exist yet" is an acceptable state | Enforced |
| P4 | No GPU/Windows hardware in the current dev environment hides platform bugs | Medium | Certain | CI matrix planned (Windows + Linux, sanitizer runs); hardware runners for GPU tests; sandbox limits stated in every report | Accepted |
| P5 | Rust/C++ boundary churn (no Rust toolchain in current sandbox) | Low | Certain | Rust crates deferred; narrow C ABI reserved for the interop boundary when Rust lands | Accepted |
