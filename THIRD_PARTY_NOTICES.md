# Third-party notices

This file lists third-party material used to build or run SYNTH, and how each item is handled.
It is copied into both release packages. It is not a license for SYNTH itself.

**Project license:** SYNTH's own code is licensed under the GNU GPL v3.0 (root `LICENSE`, also
shipped at the root of both release packages). Third-party components retain their original
licenses and notices. `LICENSES/DIALECTIC-MIT.txt` is the MIT notice of the Dialectic material
that SYNTH reuses; that material keeps its MIT notice, and the file is not a separate grant by
the SYNTH authors. The flat `SYNTH.dll` compiles in GPL-3.0-or-later code (see below), so any
binary distribution of that DLL must also meet those terms. See
[docs/PUBLICATION-CHECKLIST.md](docs/PUBLICATION-CHECKLIST.md).

## Shipped in release packages

The notice texts are verbatim license text from the pinned dependency checkouts, the vcpkg
install tree (`share/<port>/copyright`) and, for flat spdlog, the official upstream v1.16.0
`LICENSE`. Only trailing whitespace was removed; every word, copyright line and clause is
unchanged.
The release audit rejects a package that lacks any of them, or whose root `LICENSE` is not the
unmodified GPLv3 text.

| Component | Use | License | Notice file in package |
|---|---|---|---|
| [Dialectic](https://github.com/Dwemer-Dynamics/Dialectic) (Dwemer Dynamics) | Design and engine-independent client baseline; provenance in `provenance/REFERENCES.md` | MIT | `LICENSES/DIALECTIC-MIT.txt` (both lanes) |
| [libxse/CommonLibF4](https://github.com/libxse/CommonLibF4) at `6266ecc9014b473fc6b6efd04abac324477c63cd`, with its `lib/commonlib-shared` submodule | Flat `SYNTH.dll` engine bindings, compiled in | GPL-3.0-or-later WITH the Modding Exception and the GPL-3.0 Linking Exception (with Corresponding Source), relicensed upstream in `0d96d54`. Originally based on MIT code. | `LICENSES/CommonLibF4-GPL-3.0-or-later.txt`, `LICENSES/CommonLibF4-EXCEPTIONS.txt`, and the retained original notice `LICENSES/CommonLibF4-MIT.txt` (flat) |
| [spdlog](https://github.com/gabime/spdlog) | Logging. Flat: v1.16.0 static library from xmake, built with `std_format`, so it does not use fmt. VR: vcpkg 1.17.0, statically linked through the `x64-windows-static-md` triplet | MIT | Flat: `LICENSES/spdlog-1.16.0-MIT.txt`, the official v1.16.0 `LICENSE` text ("Copyright (c) 2016 Gabi Melman."). VR: `LICENSES/spdlog-MIT.txt`, the vcpkg 1.17.0 port `copyright` text ("Copyright (c) 2016 - present, Gabi Melman and spdlog contributors."). The two texts differ, so each lane ships the notice for the version it links. |
| [ArthurHub/CommonLibF4VR](https://github.com/ArthurHub/CommonLibF4VR) at `1c7b4fc860261eabad9f044e336965c26abe8ee6` | VR `SYNTHVR.dll` engine bindings, compiled in | MIT | `LICENSES/CommonLibF4VR-MIT.txt` (VR) |
| [Fallout VR Address Library](https://github.com/alandtse/fallout_vr_address_library) v1.13.1 | VR address IDs. The user installs the runtime database separately. | MIT | `LICENSES/VRAddressLibrary-MIT.txt` (VR) |
| [fmt](https://github.com/fmtlib/fmt) (vcpkg 12.2.0) | spdlog formatting in the VR lane, statically linked through the `x64-windows-static-md` triplet | MIT | `LICENSES/fmt-MIT.txt` (VR) |
| [rapidcsv](https://github.com/d99kris/rapidcsv) (vcpkg 8.99) | Header-only CSV reader compiled into CommonLibF4VR `REL/IDDB.cpp` | BSD-3-Clause | `LICENSES/rapidcsv-BSD-3-Clause.txt` (VR) |
| [rsm-mmio](https://github.com/Ryan-rsm-McKenzie/mmio) (vcpkg 2.0.0) | Memory-mapped I/O for CommonLibF4VR, static `mmio.lib` | MIT | `LICENSES/rsm-mmio-MIT.txt` (VR) |

**VR linking:** `scripts/build-vr.ps1` builds the vcpkg dependencies with the
`x64-windows-static-md` triplet (static libraries, dynamic MSVC runtime matching `/MD`). spdlog,
fmt, rsm-mmio and rapidcsv are compiled into `SYNTHVR.dll`, so it imports no `spdlog.dll` or
`fmt.dll` and the VR package ships no third-party DLLs. The earlier dynamic `x64-windows`
build imported both DLLs; see [docs/HANDOFF.md](docs/HANDOFF.md).

## Build-time dependencies

| Component | Use | License | Status |
|---|---|---|---|
| vcpkg manifest `vcpkg.json` (baseline `8e88484`): `rapidcsv`, `rsm-mmio`, `spdlog` | Supplies the VR CMake build. `rapidcsv` has no SYNTH source reference but CommonLibF4VR needs it (above). The flat xmake build does not use this manifest. | See above | Notices shipped in the VR package. |
| [DCCStudios/F4SEMenuFramework](https://github.com/DCCStudios/F4SEMenuFramework) consumer headers | Optional flat in-game settings pages, called at runtime through `GetProcAddress` | **None published** (`"license": null` at the pinned revision) | Fetched into the ignored `out/dependencies/` only and **not redistributed**. See `provenance/F4SEMENUFRAMEWORK.md`. License clarification remains open. |
| [Caprica](https://github.com/Orvid/Caprica) v0.1.5 (hash-pinned) | Compiles the original `SYNTHNative` and `SYNTHWait` Papyrus scripts | Upstream license | Build tool only. It is not shipped. |

## Required user installs (not shipped)

Fallout 4 / Fallout 4 VR, F4SE / F4SEVR, Address Library for F4SE Plugins, the VR Address
Library runtime files and the optional F4SE Menu Framework are installed separately by the user.
SYNTH packages contain no Bethesda, Creation Club, F4SE or other third-party binaries, plugins,
scripts or archives.

## SYNTH-authored game content

The flat package ships original SYNTH content: `Data/SYNTH.esp` (wait and restraint records
generated by `scripts/build_gameplay.py`), and the compiled `SYNTHWait` and `SYNTHNative` scripts
with their tracked sources. `SYNTH.esp` lists `Fallout4.esm` as a master and refers to its
public records. It does not copy Bethesda assets. Fallout and related names are trademarks of
their respective owners. SYNTH is not endorsed by Bethesda Softworks.
