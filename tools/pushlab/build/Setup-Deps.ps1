<#
  Local build dependencies for the PushFPS fork, mirroring .github/workflows/build-windows.yml, all on F:.
  Re-runnable: finished parts are skipped. Leaves out the optional NVIDIA Optical Flow headers and PushFlow
  (ONNX Runtime) - the lab does not use them.
#>
$ErrorActionPreference = 'Stop'
$src = 'F:\Dev\PushFPS'
$deps = 'F:\DevCaches\pushfps-deps'
$env:TEMP = 'F:\Temp'; $env:TMP = 'F:\Temp'
$env:VCPKG_DEFAULT_BINARY_CACHE = 'F:\DevCaches\vcpkg-binary'
$env:VCPKG_DOWNLOADS = 'F:\DevCaches\vcpkg-downloads'
$env:VCPKG_VISUAL_STUDIO_PATH = 'F:\DevTools\VS2022BuildTools'   # CI toolset; otherwise vcpkg picks VS 2026 on C: and the link fails
New-Item -ItemType Directory -Force $deps, $env:VCPKG_DEFAULT_BINARY_CACHE, $env:VCPKG_DOWNLOADS | Out-Null
Set-Location $deps
function Step($t) { Write-Host "`n=== $t  $(Get-Date -Format HH:mm:ss)" }
function Check { if ($LASTEXITCODE -ne 0) { throw "failed (exit $LASTEXITCODE)" } }
$bash = 'C:\Program Files\Git\bin\bash.exe'

# pinned versions from the workflow
$wf = Get-Content "$src\.github\workflows\build-windows.yml" -Raw
function Pin($name) { [regex]::Match($wf, "(?m)^\s+${name}:\s*(\S+)").Groups[1].Value }
$VCPKG_COMMIT = Pin 'VCPKG_COMMIT'; $DLSS_TAG = Pin 'DLSS_TAG'; $NVCODEC_TAG = Pin 'NVCODEC_TAG'; $CUDART_ZIP = Pin 'CUDART_ZIP'
$XESS_TAG = Pin 'XESS_TAG'; $FFX1_TAG = Pin 'FFX1_TAG'; $FFX2_TAG = Pin 'FFX2_TAG'; $AMF_COMMIT = Pin 'AMF_COMMIT'
$CHIAKI_COMMIT = Pin 'CHIAKI_COMMIT'; $MOONLIGHT_COMMIT = Pin 'MOONLIGHT_COMMIT'
"vcpkg $VCPKG_COMMIT dlss $DLSS_TAG xess $XESS_TAG chiaki $CHIAKI_COMMIT"

# MSVC 2022 environment (same toolset as CI)
Step 'MSVC 2022 environment'
$vcvars = 'F:\DevTools\VS2022BuildTools\VC\Auxiliary\Build\vcvars64.bat'
cmd /c "`"$vcvars`" >nul && set" | ForEach-Object { if ($_ -match '^([^=]+)=(.*)$') { Set-Item "env:$($matches[1])" $matches[2] } }
(Get-Command cl.exe).Source

Step 'Python tools (venv on F:)'
if (-not (Test-Path "$deps\venv\Scripts\python.exe")) { uv venv "$deps\venv" --python 3.12; Check }
uv pip install --python "$deps\venv\Scripts\python.exe" -q aqtinstall ninja protobuf; Check
$env:PATH = "$deps\venv\Scripts;$env:PATH"

Step 'Qt 6.8.3'
if (-not (Test-Path "$deps\Qt\6.8.3\msvc2022_64\bin\qmake.exe")) { & "$deps\venv\Scripts\aqt.exe" install-qt windows desktop 6.8.3 win64_msvc2022_64 -O "$deps\Qt"; Check }

Step 'DLSS SDK, NVENC headers, CUDA header, XeSS, FidelityFX, AMF'
$sdk = @"
set -e
cd /f/DevCaches/pushfps-deps
[ -f dlss/include/nvsdk_ngx.h ] || { rm -rf dlss; GIT_LFS_SKIP_SMUDGE=1 git clone -q --depth 1 --branch "$DLSS_TAG" --filter=blob:none --sparse https://github.com/NVIDIA/DLSS.git dlss; git -C dlss sparse-checkout set include lib/Windows_x86_64/x64; }
[ -d nv-codec-headers ] || git clone -q --depth 1 --branch "$NVCODEC_TAG" https://github.com/FFmpeg/nv-codec-headers.git nv-codec-headers
[ -f cudart-root/include/cuda.h ] || { rm -rf cudart cudart-root; curl -sSfL "$CUDART_ZIP" -o cudart.zip && unzip -q cudart.zip -d cudart && mv cudart/*/ cudart-root; rm -f cudart.zip; }
[ -d xess ] || { gh release download "$XESS_TAG" -R intel/xess -p '*.zip' -O xess.zip && unzip -q xess.zip -d xess && rm -f xess.zip; }
[ -d ffx2 ] || { GIT_LFS_SKIP_SMUDGE=1 git clone -q --depth 1 --branch "$FFX2_TAG" --filter=blob:none --sparse https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git ffx2; git -C ffx2 sparse-checkout set Kits/FidelityFX/api/include Kits/FidelityFX/framegeneration/include Kits/FidelityFX/upscalers/include; }
[ -d ffx1 ] || { gh release download "$FFX1_TAG" -R GPUOpen-LibrariesAndSDKs/FidelityFX-SDK -p '*.zip' -O ffx1.zip && unzip -q ffx1.zip -d ffx1 && rm -f ffx1.zip; }
[ -f amf/amf/public/include/components/FRC.h ] || { rm -rf amf; git init -q amf && git -C amf remote add origin https://github.com/GPUOpen-LibrariesAndSDKs/AMF.git; git -C amf fetch -q --depth 1 --filter=blob:none origin "$AMF_COMMIT"; git -C amf sparse-checkout set amf/public/include && git -C amf checkout -q FETCH_HEAD; }
echo sdk ok
"@
& $bash -c $sdk; Check

Step 'FidelityFX 1.1.4 optical flow (static)'
$ffx1sdk = Get-ChildItem "$deps\ffx1" -Directory -Recurse -Filter sdk | Select-Object -First 1
if (-not (Get-ChildItem "$($ffx1sdk.FullName)\bin" -Recurse -Filter ffx_opticalflow_x64.lib -ErrorAction SilentlyContinue)) {
    Push-Location $ffx1sdk.FullName
    cmake -S . -B build -A x64 -DFFX_API_BACKEND=DX12_X64 -DFFX_ALL=OFF -DFFX_OF=ON -DFFX_AUTO_COMPILE_SHADERS=1 -DCMAKE_POLICY_DEFAULT_CMP0091=NEW -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded; Check
    cmake --build build --config Release --parallel; Check
    Pop-Location
}
$xessRoot = (Get-ChildItem "$deps\xess" -Recurse -Filter xefg_swapchain_d3d12.h | Select-Object -First 1).Directory.Parent.Parent.FullName
$ffx1lib = Get-ChildItem "$($ffx1sdk.FullName)\bin" -Recurse -Filter ffx_opticalflow_x64.lib | Select-Object -First 1
$ffx1Root = $ffx1lib.Directory.Parent.Parent.FullName
"XeSS root $xessRoot ; FFX1 root $ffx1Root"

Step 'vcpkg + FFmpeg 9.0.1'
if (-not (Test-Path "$deps\vcpkg\vcpkg.exe")) {
    if (-not (Test-Path "$deps\vcpkg\.git")) { git init -q vcpkg; git -C vcpkg remote add origin https://github.com/microsoft/vcpkg.git }
    git -C vcpkg fetch -q --depth 1 origin $VCPKG_COMMIT; Check; git -C vcpkg checkout -q FETCH_HEAD; Check
    & "$deps\vcpkg\bootstrap-vcpkg.bat" -disableMetrics; Check
}
$env:VCPKG_BUILDTREES_ROOT = 'F:\DevCaches\vcpkg-buildtrees'
& "$deps\vcpkg\vcpkg.exe" install "ffmpeg[core,avcodec,avformat,avfilter,swresample,swscale]:x64-windows" --clean-after-build "--x-buildtrees-root=$env:VCPKG_BUILDTREES_ROOT"; Check

Step 'Streaming dependencies (static)'
& "$deps\vcpkg\vcpkg.exe" install --classic --triplet x64-windows-static "--x-install-root=$deps\stream-installed" "--overlay-ports=$src\scripts\xbox\vcpkg-overlay" `
    openssl opus json-c libevent miniupnpc protobuf sdl3 "libdatachannel[srtp,ws]" nlohmann-json pkgconf zlib --clean-after-build "--x-buildtrees-root=$env:VCPKG_BUILDTREES_ROOT"; Check

Step 'Streaming sources (Chiaki-ng with patches, moonlight-common-c)'
# All reviewed patches, in name order (as CI); the stage is rebuilt whenever the set or their content changes.
$patches = @(Get-ChildItem "$src\scripts\remoteplay\patches\*.patch" | Sort-Object Name)
$patchStamp = "$deps\chiaki-stage.patches"
$wantStamp = ($patches | ForEach-Object { "$($_.Name) $((Get-FileHash $_.FullName).Hash)" }) -join ';'
$haveStamp = if (Test-Path $patchStamp) { (Get-Content $patchStamp -Raw).Trim() } else { '' }
if ((Test-Path "$deps\chiaki-stage") -and $haveStamp -ne $wantStamp) {
    git -C chiaki-clean worktree remove --force "$deps\chiaki-stage" 2>$null
    Remove-Item "$deps\chiaki-stage" -Recurse -Force -ErrorAction SilentlyContinue
    git -C chiaki-clean worktree prune
}
if (-not (Test-Path "$deps\chiaki-stage\CMakeLists.txt")) {
    if (-not (Test-Path "$deps\chiaki-clean\CMakeLists.txt")) {
        Remove-Item "$deps\chiaki-clean" -Recurse -Force -ErrorAction SilentlyContinue
        git -c core.autocrlf=false clone -q https://github.com/streetpea/chiaki-ng.git chiaki-clean; Check
        git -C chiaki-clean -c advice.detachedHead=false checkout -q $CHIAKI_COMMIT; Check
        git -C chiaki-clean -c core.autocrlf=false submodule update -q --init --recursive; Check
    }
    git -C chiaki-clean worktree add -q --detach "$deps\chiaki-stage" $CHIAKI_COMMIT; Check
    git -C chiaki-stage -c core.autocrlf=false submodule update -q --init --recursive; Check
    foreach ($p in $patches) { git -C chiaki-stage apply $p.FullName; Check }
    Set-Content $patchStamp $wantStamp
}
& "$deps\venv\Scripts\python.exe" "$src\scripts\remoteplay\verify-chiaki-stage.py" --stage "$deps\chiaki-stage" --clean "$deps\chiaki-clean"; Check
if (-not (Test-Path "$deps\moonlight-common-c\CMakeLists.txt")) {
    git -c core.autocrlf=false clone -q https://github.com/moonlight-stream/moonlight-common-c.git moonlight-common-c; Check
    git -C moonlight-common-c -c advice.detachedHead=false checkout -q $MOONLIGHT_COMMIT; Check
    git -C moonlight-common-c -c core.autocrlf=false submodule update -q --init --recursive; Check
}
& "$deps\venv\Scripts\python.exe" "$src\scripts\moonlight\verify-moonlight-stage.py" --source "$deps\moonlight-common-c" --lock "$src\scripts\moonlight\dependency-lock.json"; Check

$roots = [ordered]@{ XESS_ROOT = $xessRoot; FFX1_ROOT = $ffx1Root }
$roots | ConvertTo-Json | Set-Content "$deps\roots.json"
Step 'dependencies ready'
