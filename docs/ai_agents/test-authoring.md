# Test authoring

A test earns its place by catching a defect that no cheaper check would catch.

## When to add a test

- A change fixes a demonstrated defect. Exercise the repaired call path and
  choose an assertion that fails without the fix.
- New behavior adds a branch, guest byte layout, persisted state, or failure
  path that existing tests do not reach.
- Ownership changes across a callback, memory watch, or shutdown boundary.
  Check stale tokens, released owners, and partial failure where they apply.
- Configuration changes precedence or persistence. Check saved bytes and the
  effective value, including values that must remain local to the session.

## When not to add a test

- Another test already reaches the same branch with the same assertions.
  A different profile name or package ID alone does not justify another case.
- The test assigns structure fields and only reads those fields back.
- Expected values come from the same layout or codec used by the implementation.
  Keep independent byte vectors; round trips alone cannot prove a wire layout.
- A guard rejects the input before the intended behavior runs.
- The change only alters prose or formatting. Use the checks in
  [Development](../development.md#check-the-changed-surface).

## Choose the cheapest fixture that proves the claim

Pick the first level that can detect the defect:

1. Literal inputs to a pure helper or decoder, as in
   [window sizing](../../tests/unit/ui/window_size_test.cpp).
2. An in-memory model with supplied time and deterministic inputs, as in the
   relay `Core` [fixture](../../tests/unit/core/relay_test.cpp).
3. A bounded Lua run through `RunModScript`, as in the
   [Lua tests](../../tests/unit/system/mod_script_test.cpp).
4. A temporary file tree for discovery, persistence, or path behavior, as in
   [profile tests](../../tests/unit/system/profile_test.cpp).
5. Shared guest memory and a small fake for guest pointers or protected writes,
   as in [memory watches](../../tests/unit/memory/physical_write_watch_test.cpp).
6. Runtime, application, socket, or device integration when the defect crosses
   that boundary; use the [detached UI tests](../../tests/unit/ui/detached_presentation_test.cpp)
   or [relay client tests](../../tests/unit/kernel/xam/relay_system_link_test.cpp).
7. An [installed consumer](../../tests/installed_cpu_consumer/README.md) when
   the installed headers, exports, or package are the contract.

## Products and matrices

Choose representatives by branch, format, byte boundary, and ownership route.
Keep every explicitly admitted texture tuple and its independent expected
extents in a data matrix. See the
[Prepared tests](../../tests/unit/graphics/prepared_resource_input_test.cpp)
for separate linear, tiled, and cube families.

Repeat an integrated scenario only for a distinct mechanism. Do not multiply
every socket failure by every room or profile. Preserve separate checks for
missing and empty loadouts, base and mip writes, and signed companions.

When reducing a matrix, state the lost integration coverage and retain its data
assertions. Preserve captured fetch words, addresses, and expected sizes.

## Scaffolding

Register Catch2 cases in [unit CMake](../../tests/unit/CMakeLists.txt).
Use `CAPTURE` for readable failures. CTest discovers whole `TEST_CASE`s;
a `SECTION` reruns case setup.

Use [GetTestMemory](../../tests/unit/test_memory.h) in `unit_tests`.
Do not initialize another guest `Memory` there. Initialization and name
collision checks live in the separate `guest_memory_section_tests` executable.

Reuse existing builders and fakes, but keep expected literals visible.
The [Prepared helper](../../tests/unit/graphics/prepared_frame_input_test_support.h)
owns borrowed spans for its draw and copy checks.

Restore cvars, observers, and test hooks after each case. Give file fixtures
unique paths and release their resources even when a requirement fails.
Keep allocator injection in `mod_script_tests` and
`mod_script_activation_tests`; their isolation is defined in unit CMake.

## Runs and limits

[Development](../development.md#build-test-and-install-on-windows) owns setup
and the full CTest commands. Tests require `REXGLUE_BUILD_TESTS=ON`.
The [local fast preset](../../CMakePresets.json) omits network, PPC, and CMake
staging checks. From the repository root, after building Release, run:

```text
ctest --preset win-amd64-local-fast --output-on-failure --no-tests=error
```

Run the full applicable suite for behavior changes.
Tag cases that bind wildcard or non-loopback addresses `[.network]`;
`REXGLUE_NETWORK_TESTS=ON` registers them. GPU presenter checks require
`REX_RUN_D3D12_GPU_TESTS=1`; inspect skips before claiming coverage.
[PowerPC tests](../../tests/ppc/CMakeLists.txt) generate cases from assembly.
Edit their inputs. [Unix CI](../../.github/workflows/_build-platform.yaml)
runs the mod-plugin selection.
An SDK test pass does not establish title runtime acceptance.
