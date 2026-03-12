# MI3DI – 3D MIDI Synthesizer Driver

**MI3DI** (version 1.0.0) is a Windows virtual MIDI output device that renders
SF2 / SFZ soundfonts in real-time and spatializes **every individual note** as
its own 3D audio source using **OpenAL Soft** and the **EFX** extension.

## How 3D positioning works

| MIDI property | Maps to | 3D axis |
|---|---|---|
| Pan (CC #10, 0–127) | −10 m … +10 m | **X** (left/right) |
| Note number (0–127) | −10 m … +10 m | **Y** (low/high) |
| Volume (CC #7, 0–127) | 100 m … 1 m distance | **Z** (far/near) |

Note velocity is **not** used for gain — the perceived volume of each note
comes entirely from OpenAL's inverse-distance attenuation model, giving an
authentic 3D soundstage.

## EFX effects

| MIDI CC | Effect |
|---|---|
| CC #91 (Reverb Depth) | EAX Reverb send level |
| CC #93 (Chorus Depth) | Chorus send level |

Both effects use OpenAL Soft's native EFX slots and require an OpenAL Soft
build with EFX support (the bundled `OpenAL32.dll` includes it).

## Requirements

- Windows 7 or later (32-bit or 64-bit)
- A GM/GS/XG-compatible SF2 soundfont **or** an SFZ instrument
- OpenAL Soft (bundled in the release zip as `OpenAL32.dll`)

## Installation

1. Download the zip for your Windows architecture from
   [Releases](../../releases).
2. Extract all files into a temporary folder.
3. Place your soundfont file in that folder and rename it `default.sf2`
   (or `default.sfz` for SFZ). Alternatively set the environment variable
   `MI3DI_SOUNDFONT=C:\path\to\your.sf2`.
4. Right-click `install.bat` → **Run as administrator**.
5. Open any MIDI application; **MI3DI 3D MIDI Synth** will appear as an
   output device.

## Soundfont search order

1. `MI3DI_SOUNDFONT` environment variable
2. `%APPDATA%\MI3DI\default.sf2` / `.sfz`
3. Directory containing `mi3di.dll` → `default.sf2` / `default.sfz`,
   `GeneralUser GS v1.471.sf2`, `FluidR3_GM.sf2`
4. `%WINDIR%\System32\mi3di.sf2` / `.sfz`

## Logging (verbose)

Set `MI3DI_LOG` to a writable file path before launching the MIDI host:

```bat
set MI3DI_LOG=C:\temp\mi3di.log
```

The log includes the version string but no timestamps (to keep diffs clean).

## Uninstall

Right-click `uninstall.bat` → **Run as administrator**.

## Building from source

### Prerequisites

- MSYS2 with MinGW-w64 toolchain (x86_64 or i686)
- CMake ≥ 3.16
- `mingw-w64-{arch}-openal` package

```bash
# 64-bit
pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake \
          mingw-w64-x86_64-ninja mingw-w64-x86_64-openal

mkdir build64 && cd build64
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja
cpack -G ZIP
```

For 32-bit, use `mingw-w64-i686-*` packages inside a MINGW32 shell.

The GitHub Actions workflow (`.github/workflows/build.yml`) builds both
architectures automatically on every push and produces versioned zip
artefacts.

## License

MI3DI is released under the **GNU General Public License v3.0 or later**.
See [LICENSE](LICENSE) for details.
