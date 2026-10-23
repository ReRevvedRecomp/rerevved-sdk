# SDK Style Guide

ReXGlue upstream remains the baseline for SDK code style. This page records the
maintained ReRevved fork's local conventions and scope. Apply it to maintained
SDK code and documentation; generated and third-party content follows its own
source and ownership rules.

The canonical policy and configuration files are:

- [Upstream contributing
  guide](https://github.com/rexglue/rexglue-sdk/wiki/Development/Contributing)
  for prerequisites, contribution workflow, and upstream conventions.
- [Maintaining the ReRevved fork](MAINTAINING.md) for upstream integration,
  branch, artifact, and release policy.
- [`.clang-format`](../.clang-format), [`.clang-tidy`](../.clang-tidy),
  [`.cmake-format.yaml`](../.cmake-format.yaml), and
  [`.editorconfig`](../.editorconfig) for mechanical formatting and lint rules.

## C++

### Names and files

- Use lower snake case for namespaces, source and header filenames, and
  directories. Use the existing namespace spelling in the surrounding module;
  both nested namespace blocks and C++17 nested namespace syntax are present.
- Use `CamelCase` for classes, structs, enums, functions, methods, and type
  aliases. Use lower snake case for local variables, parameters, and data
  members. Private and class data members generally carry a trailing `_`; public
  value structs such as
  [`AssetOverlayPackage`](../include/rex/system/asset_overlay_catalog.h) expose
  bare snake_case fields. Existing accessors also use snake_case in
  upstream APIs; preserve those public names.
- Use a `k` prefix with `CamelCase` for new constants and enum values. Some
  public enums use bare `CamelCase` values; preserve their names.
  Use `UPPER_CASE` for macro definitions.
- Keep the `rex` root namespace for SDK facilities. Title-specific behavior
  belongs in the title repository rather than in a general SDK module.

### Formatting

- Use C++23 as selected by the CMake project. Indent with two spaces and never
  use tabs. Use attached braces (`if (...) {`) and `Type* value` or `Type&
  value` pointer and reference placement.
- Keep source lines within the configured 100-column limit. Let clang-format
  handle continuation indentation and wrapping; it uses four spaces for
  continuations, aligned brackets, and packed arguments and parameters where
  they fit.
- Preserve the local include order. `.clang-format` keeps include sorting off,
  regroups include blocks, and defines the repository's categories: the main
  header, C and C++ standard headers, platform headers, third-party headers,
  then `<rex/...>` headers.
- Keep short inline functions in the form accepted by `.clang-format`. Match
  nearby upstream code for one-line control statements and empty
  virtual hooks; use a block when the body or its reason needs room.

### Comments

Use short inline comments for one enum value, field, list, local invariant,
lifetime rule, platform detail, or compatibility constraint. Put a shared
explanation immediately above the group it describes. Put longer contracts in
the relevant public header or this guide, then link that canonical explanation
from entry-point documentation. Keep a `TODO` or `FIXME` with the affected code
when an action is still required. Preserve existing upstream license, copyright,
and provenance blocks.

### Headers and APIs

Headers under `include/rex` and the exported CMake package are public SDK
surface. Keep public headers small, stable, and self-sufficient: include what a
declaration directly needs, use `#pragma once`, and forward declare only when
that keeps the dependency boundary clear. Document non-obvious API behavior,
ownership, thread or lifetime requirements, and error results at the
declaration. Check [Maintaining the ReRevved fork](MAINTAINING.md) and the
[release rules](RELEASING.md) before changing public API or ABI contracts.

### Errors and ownership

- Use `rex::Result<T>` or `rex::VoidResult` with `rex::Error` for operations
  that need structured failure. Return `rex::Ok(...)` or `rex::Err(...)` and
  propagate an error early with the existing `TRY` helper where it fits the
  surrounding API.
- Preserve established boundary contracts. Existing modules return `bool`,
  platform status values, or subsystem result enums where those APIs already use
  them. Use the repository assertion helpers for violated invariants and
  `FatalError` only for paths that must terminate.
- Return `std::unique_ptr` from factories that transfer one owner. Use
  `std::shared_ptr` where the current API has a shared lifetime, as with the
  console presentation sink. Pass borrowed references, spans, and views when the
  surrounding API does so; make ownership and lifetime clear at public
  declarations.

## CMake and scripts

- Format CMake with two spaces, a 100-column line width, lowercase command
  names, and uppercase keywords as configured in
  [`.cmake-format.yaml`](../.cmake-format.yaml). Keep platform and test
  configuration in `CMakePresets.json` and use existing targets and options
  rather than introducing ad hoc build commands.
- PowerShell scripts use four spaces and LF line endings. Shell scripts use two
  spaces and LF line endings. The `scripts/PSReX` module is the supported
  wrapper for configure, build, test, install, format, lint, and setup commands.
- Keep generator and build logic in CMake or the existing script entry points.
  Keep source comments focused on the local reason; put reusable command
  explanations in this guide or the owning build document.

## Generated and vendored code

Files marked `Auto-generated` or `DO NOT EDIT` are outputs. Change their source
template or generator instead. This includes the CMake-configured
`include/rex/version.h`, embedded templates, PPC test outputs, and generated
project/codegen files. Build output belongs under `out/` or the configured build
tree and is not committed.

The `thirdparty/` tree contains vendored dependencies. Preserve each
dependency's attribution, license, and submodule boundary; update it through the
dependency's own process. Project formatting and lint checks cover maintained
`include`, `src`, and `tests` files, not vendored source.

## Verification

Use the narrowest existing check for the changed surface. From PowerShell,
import the module before using its aliases:

```powershell
Import-Module .\scripts\PSReX
```

For a source or header edit, `rex-format` formats in place. Use `rex-format
-All` for all maintained C/C++ files or pass explicit paths for a bounded edit.
The read-only CI and pre-commit checks use `clang-format --dry-run --Werror`;
`cmake-format --check` is used for staged CMake files when `cmake-format` is
installed. `rex-lint` is read-only and requires `compile_commands.json`; it
applies [`.clang-tidy`](../.clang-tidy) to `include` and `src`. Enable the
compile database explicitly when configuring:

```text
cmake --preset <platform> -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

The wrapper checks that the database exists, but does not aggregate individual
`clang-tidy` failures. Inspect the diagnostic output and each tool's result; the
final `Done.` message does not mean lint checks passed.

The formatter resolves the pinned version from
[`scripts/requirements-dev.txt`](../scripts/requirements-dev.txt).

Use [Development](development.md) for commands to build, test, install, and run
focused checks. It also explains the differences between the direct CMake
commands and PowerShell wrappers. Documentation-only edits need link inspection
and `git diff
--check`; they do not require an SDK build, install, source formatter, or source
lint run.

The [lint workflow](../.github/workflows/lint.yaml) is the CI formatting gate.
The [platform workflow](../.github/workflows/_build-platform.yaml) is the
canonical build, install, and test sequence.
