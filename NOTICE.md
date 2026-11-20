# PushFPS notice

PushFPS: console frame generation from a capture card.
Copyright (C) 2026 DosukaSOL and the PushFPS contributors.

PushFPS is a **modified version of Veyra** (https://github.com/Likely7/Veyra-NRVideo),
Copyright (C) Likely7 and the Veyra contributors.

## Modifications (GPL-3.0 section 5a)

PushFPS was forked from Veyra on **2026-11-12** and has been modified by the PushFPS contributors since
then (latest change: see the date of the newest commit). The modifications, in short:

- Product name, logo, icon, update channel and links (`include/veyra/Brand.h` and the files that use it).
- Interface: quick setup with console and game profiles, home screen layout, row layout and theme
  (`qml/Veyra/PushQuickSetup.qml`, `HomePage.qml`, `VRow.qml`, `Theme.qml`, `ProPage.qml`, `TopDock.qml`
  and others), with their translations in `i18n/catalog.json`.
- Builds without the NVIDIA Optical Flow SDK (`src/ngx/NvOfSessionUnavailable.cpp`); release builds with the
  SDK's headers from a private secret compile Veyra's own NVIDIA optical flow code unchanged.
- Windows installer and release packaging (`packaging/`), build and test workflows (`.github/workflows/`).
- OBS Studio plugin (`obs-plugin/`).
- Picture & sound editor, sound profiles and the saved-recording card (`qml/Veyra/PushPicture.qml`,
  `PushSoundProfile.qml`, `PushRecordingSaved.qml`, `src/sink/SoundProfile.cpp`).
- NVIDIA Image Scaling as a super-resolution choice (`src/pipeline/NisPass.cpp`, `shaders/Nis*.hlsl`).
- Optimise for my PC, game presets, latency readout, Discord sharing and the capture card list
  (`include/veyra/engine/PushBenchmarkProbe.h`, `src/ui/bridge/PushFps.cpp`, `qml/Veyra/PushOptimizeDialog.qml`,
  `PushGamePresetsDialog.qml`, `PushGamePresets.js`, `PushLatencyBadge.qml`, `PushCaptureCards.js`).
- ANR frame generation as in Veyra 2.1.6: no ANR file ships; usable once the user adds ANR's own files.

The complete, dated list of changes is the Git history of https://github.com/DosukaSOL/PushFPS, starting
after Veyra's commit that PushFPS was forked from.

## License

PushFPS is free software: you can redistribute it and/or modify it under the terms of the GNU General
Public License version 3 as published by the Free Software Foundation (see [LICENSE](LICENSE)). The files
written for PushFPS are additionally available under any later version of that license.

PushFPS is distributed in the hope that it will be useful, but **WITHOUT ANY WARRANTY**; without even the
implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
for more details.

PushFPS release builds (from 2.4.0) include Veyra's streaming sources:

- **PS5 Remote Play** compiles [chiaki-ng](https://github.com/streetpea/chiaki-ng) at
  `0e16950165f06e5c3291537c2eeba6e852be7120` with Veyra's two patches (`scripts/remoteplay/patches`). That part
  is also under the AGPL-3.0 with the upstream OpenSSL exception; see [licenses/remoteplay](licenses/remoteplay).
- **PC streaming** compiles [moonlight-common-c](https://github.com/moonlight-stream/moonlight-common-c) (GPL-3.0)
  at `f900dd4767759c7b9d0e93bcea666b55c69ea62f` with its pinned ENet and nanors (MIT).
- **Xbox streaming** links [libdatachannel](https://github.com/paullouisageneau/libdatachannel) (MPL-2.0) and its
  dependencies, built by vcpkg with Veyra's libjuice overlay (`scripts/xbox/vcpkg-overlay`).

The static libraries (OpenSSL, Opus, SDL3, curl, json-c, libevent, miniupnpc, nanopb, Jerasure, gf-complete,
libjuice, usrsctp, libsrtp, plog, nlohmann-json) come from vcpkg at the commit pinned in the build workflow. Each
release attaches a *streaming-source* archive with the exact sources and recipes used; their notices are in
[licenses](licenses) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Additional permission under GNU GPL version 3 section 7

This permission is given by the copyright holders of the parts written for PushFPS (DosukaSOL and the
PushFPS contributors). It applies to those parts only; it does not change the terms of the parts written
by Veyra's authors (see below).

If you modify PushFPS, or any covered work, by linking or combining it with any of the following, containing
parts covered by the terms of their own licenses: the NVIDIA RTX SDKs (DLSS SDK and NGX SDK), the NVIDIA
Video Effects and Video Frame Generation SDKs, the NVIDIA Optical Flow SDK, the NVIDIA CUDA driver API
headers, the Intel XeSS SDK, the AMD FidelityFX SDK, Microsoft DirectML (used by PushFlow through ONNX Runtime),
or the unmodified runtime libraries and models these SDKs provide (together, "the SDKs"), the licensors of PushFPS grant you additional permission to convey the
resulting work. Corresponding Source for a non-source form of such a combination does not need to include
the source code of the parts of the SDKs used, but shall include the source code of PushFPS. The SDKs remain
under their own licenses, and nothing here places them under the GPL.

**Veyra's code.** PushFPS.exe also contains code written by Veyra's authors, which they license under
GPL-3.0. Veyra's author builds and distributes Veyra with the same SDKs, and PushFPS distributes the same
combination. A permission like the one above for Veyra's code can only come from Veyra's copyright holders.

## NVIDIA SDK attribution

PushFPS.exe incorporates the NVIDIA DLSS SDK and NGX SDK (version 310.7.0) under the NVIDIA RTX SDKs License,
whose text is in `licenses/sdk` of every release. NVIDIA, NVIDIA RTX and DLSS are trademarks of NVIDIA
Corporation. PushFPS is not sponsored or endorsed by NVIDIA. The app shows this attribution under
*Settings, About*.

PushFPS also includes the shader source of the NVIDIA Image Scaling SDK 1.0.3 (Copyright (c) 2022 NVIDIA
CORPORATION & AFFILIATES) under the MIT License, in `third_party/nis`; the licence text is
`licenses/NVIDIA_Image_Scaling_SDK_MIT.txt`.

## What the installer leaves out by default

The installer downloads Veyra's runtime package from Veyra's own release. By default it installs only files
that may be passed on under their vendors' terms:

- Files that say they are from NVIDIA but are not originals with a valid NVIDIA signature (the
  community-modified DLSS build that AI picture enhancement runs on with NVIDIA RTX cards; the app lists it
  as "RTX 20–50") are not installed.
- On AMD installs, the AI enhancement runtimes that run NVIDIA's model on Radeon cards
  (`runtime\amd-nr-rdna3`, `runtime\amd-nr`) are not installed: NVIDIA licenses its DLSS runtimes for
  systems with NVIDIA graphics only. FSR and XeSS frame generation are not affected.

Nothing is removed from PushFPS or Veyra: the installer offers these "experimental extras" with a yes/no
question (default no), or installs them with `Install-PushFPS.cmd -ExperimentalExtras Yes`. A user who
chooses them gets them straight from Veyra's package, on their own decision; PushFPS does not host them.
The installer lists what it left out in `PushFPS-VERSION.txt` in the install folder. CI checks that default
installs contain no such file and that the opt-in installs them. `.github/workflows/audit-runtime.yml`
produces a full report of the runtime package (publisher, version and signature of every program file).

## Source code

The Corresponding Source of every PushFPS release is the Git tag of the same name in
https://github.com/DosukaSOL/PushFPS. The build recipe (exact dependency versions and commands) is
`.github/workflows/build-windows.yml` in that tag; the OBS plugin is built by the `obs-plugin` job of the
same workflow.

## Third-party components

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and the [licenses](licenses) folder. Vendor SDKs
(NVIDIA DLSS, Intel XeSS, AMD FidelityFX) are downloaded at build time from their publishers under their
own licenses and are not part of this repository. The runtime package the installer downloads is
distributed by Veyra's author from Veyra's GitHub releases; PushFPS does not rehost it.
