<p align="center">
  <a href="https://github.com/rexglue/rexglue-sdk">
    <img src="https://github.com/rexglue/rexglue-media/blob/main/ReX_Banner.png" alt="Upstream ReXGlue banner">
  </a>
</p>

# ReRevved SDK

The maintained ReXGlue fork for ReRevved: Xbox 360 PowerPC-to-C++ recompilation
and runtime, graphics, audio, input, and mod support.

[![License: BSD-3-Clause](https://img.shields.io/badge/license-BSD--3--Clause-blue)](LICENSE)
[![SDK artifacts](https://github.com/ReRevvedRecomp/rerevved-sdk/actions/workflows/sdk-artifacts.yaml/badge.svg)](https://github.com/ReRevvedRecomp/rerevved-sdk/actions/workflows/sdk-artifacts.yaml)
[![Upstream Discord](https://img.shields.io/badge/Discord-Upstream-5865F2?logo=discord&logoColor=white)](https://discord.gg/CNTxwSNZfT)

## Install

**To play:** use a [ReRevved
release](https://github.com/ReRevvedRecomp/rerevved/releases), not an SDK
package. Follow the [ReRevved
documentation](https://github.com/ReRevvedRecomp/rerevved/blob/main/docs/README.md)
to set up game files, adjust settings, play online, and use mods.

**To develop:** use the exact SDK commit and installed version in the title's
`rexglue-sdk.lock.json`. Upstream downloads do not contain this fork's changes.
The [SDK artifacts workflow](.github/workflows/sdk-artifacts.yaml) builds a
selected full commit SHA. See [Development](docs/development.md) to build and
install from source and [Maintaining the fork](docs/MAINTAINING.md) for artifact
selection and consumer validation.

## System requirements

For the Windows x64 source build, use Clang 18 or newer, Visual Studio C++ Build
Tools with the Windows SDK, CMake 3.25 or newer, and Ninja. Start in an x64
developer environment with the repository's submodules initialized. Python is
needed for the formatting tools. The [development
guide](docs/development.md#prerequisites) covers other presets and PowerPC test
tools; SDK presets do not establish title runtime support.

Players need their own legally obtained Xbox 360 Civilization Revolution files
and version 1.3 title update. See the title documentation for player system
requirements; the SDK is not a playable release.

## Documentation

The [documentation index](docs/README.md) groups build instructions, integration
references, networking, mods, and contribution rules by task.

> This is an unofficial project and contains no game assets. Players need
> their own legally obtained copy of Civilization Revolution. See
> [LICENSE](LICENSE).

> [!CAUTION]
> This project is in early development. Expect things to not work quite
> right and there to be significant changes and breaking public API updates
> as development progresses. Contributions and feedback are welcome, but
> please be aware that the codebase is still evolving rapidly.

## Contributing

Keep SDK changes reusable; title behavior belongs in the title repository.
Preserve upstream attribution, vendored code and dependency boundaries.

Open pull requests against `main`, the only maintained branch. Never commit
fork work to `upstream/main`; [Maintaining the fork](docs/MAINTAINING.md)
covers upstream merges and consumer locks.

Installed `include/rex` headers, exported CMake targets, and the mod and GPU
plugin interfaces are public SDK surface. Follow the [release version
rules](docs/RELEASING.md) when they change, and validate the title against the
new install before repinning it. Do not commit generated output, `out/` or
machine configuration.

Follow the [style guide](docs/style-guide.md) and [Development](docs/development.md)
for formatting, builds and tests. A pull request explains the problem, the new
behavior, any public interface change, and the tests run. Commit subjects use
`type: imperative summary`, at most 50 characters, with no body or parentheses,
a lowercase type and a lowercase opening verb. Upstream's
[CONTRIBUTING.md](CONTRIBUTING.md) covers general setup.

## Upstream resources

ReXGlue builds on [Xenia](https://github.com/xenia-project).

Upstream [releases](https://github.com/rexglue/rexglue-sdk/releases) and
[Discord](https://discord.gg/CNTxwSNZfT) are maintained by ReXGlue. These
downloads are upstream SDK builds; use the fork artifacts for ReRevved.

### Upstream builds

| Channel | CI | Download |
| --- | --- | --- |
| Release | [![win-amd64](https://github.com/rexglue/rexglue-sdk/actions/workflows/build-win-amd64.yaml/badge.svg)](https://github.com/rexglue/rexglue-sdk/actions/workflows/build-win-amd64.yaml) [![linux-amd64](https://github.com/rexglue/rexglue-sdk/actions/workflows/build-linux-amd64.yaml/badge.svg)](https://github.com/rexglue/rexglue-sdk/actions/workflows/build-linux-amd64.yaml) [![linux-arm64](https://github.com/rexglue/rexglue-sdk/actions/workflows/build-linux-aarch64.yaml/badge.svg)](https://github.com/rexglue/rexglue-sdk/actions/workflows/build-linux-aarch64.yaml) | [Latest stable](https://github.com/rexglue/rexglue-sdk/releases/latest) |
| Nightly | [![nightly](https://github.com/rexglue/rexglue-sdk/actions/workflows/nightly.yaml/badge.svg)](https://github.com/rexglue/rexglue-sdk/actions/workflows/nightly.yaml) | [Latest pre-release](https://github.com/rexglue/rexglue-sdk/releases?q=prerelease%3Atrue) |

## Licensing

See [LICENSE](LICENSE).

The ReXGlue SDK fork is BSD-3-Clause software by the ReXGlue and Xenia authors.

Civilization Revolution names and marks belong to their owners; ReRevved is
unaffiliated.

## Disclaimer

ReXGlue is not affiliated with nor endorsed by Microsoft or Xbox. It is an
independent project created for educational and development purposes. All
trademarks and copyrights belong to their respective owners.

This project is not intended to promote piracy nor unauthorized use of
copyrighted material. Any misuse of this software to endorse or enable this type
of activity is strictly prohibited.

## Credits

### ReXGlue

- [Tom (crack)](https://github.com/tomcl7) - Project Founder
- [Loreaxe](https://github.com/Loreaxe) - Linux Contributor
- [mystixor](https://github.com/Mystixor) - Windows Contributor
- [Graine25](https://github.com/Graine25) - Project Support
- [Carlos Estrague (mrcmunir)](https://github.com/mrcmunir) - Linux / ARM64
  Contributor
- [sanjay900](https://github.com/sanjay900) - Linux / SDL Contributor
- [Toby](https://github.com/TbyDtch) - Project Support
- [Roxxsen](https://github.com/Roxxsen) - CI/CD Contributor

The list above is not exhaustive. Thanks to everyone in the ReXGlue community
who contributes code, files issues, tests builds, and keeps the project moving.

### Very Special Thank You:

- [Project Xenia](https://github.com/xenia-project/xenia/tree/master/src/xenia) - Their invaluable work on Xbox 360 emulation laid the groundwork for ReXGlue's development. This project (and numerous others) would not exist without their hard work and dedication.
- [XenonRecomp](https://github.com/hedge-dev/XenonRecomp) - For pioneering the
  modern static recompilation approach for Xbox 360. A lot of the codegen
  analysis logic and instruction translations are based on their work. Thank
  you!
- [rexdex's recompiler](https://github.com/rexdex/recompiler) - The OG static
  recompiler for Xbox 360.
- Many others in the Xbox 360 homebrew and modding communities whose work and
  research have contributed to the collective knowledge that makes projects like
  this possible.
