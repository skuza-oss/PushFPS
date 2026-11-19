<#
  Configure (first time) and build PushFPS.exe from F:\Dev\PushFPS into F:\Dev\pushfps-build\out, then optionally
  copy PushFPS.exe and the QML into the lab copy. Incremental: only changed files recompile.
  .\Build.ps1 [-Install] [-Reconfigure]
#>
param([switch]$Install, [switch]$Reconfigure)
$ErrorActionPreference = 'Stop'
$src = 'F:\Dev\PushFPS'; $deps = 'F:\DevCaches\pushfps-deps'; $out = 'F:\Dev\pushfps-build\out'
$env:TEMP = 'F:\Temp'; $env:TMP = 'F:\Temp'
function Check { if ($LASTEXITCODE -ne 0) { throw "failed (exit $LASTEXITCODE)" } }
$vcvars = 'F:\DevTools\VS2022BuildTools\VC\Auxiliary\Build\vcvars64.bat'
cmd /c "`"$vcvars`" >nul && set" | ForEach-Object { if ($_ -match '^([^=]+)=(.*)$') { Set-Item "env:$($matches[1])" $matches[2] } }
$stream = "$deps\stream-installed\x64-windows-static"
$env:PATH = "$deps\venv\Scripts;$stream\tools\protobuf;$env:PATH"
$roots = Get-Content "$deps\roots.json" -Raw | ConvertFrom-Json
$qt = "$deps\Qt\6.8.3\msvc2022_64"; $ffmpeg = "$deps\vcpkg\installed\x64-windows"
if ($Reconfigure -or -not (Test-Path "$out\build.ninja")) {
    $env:PKG_CONFIG_PATH = "$stream\lib\pkgconfig"
    cmake -S $src -B $out -G Ninja -DCMAKE_BUILD_TYPE=Release `
        "-DCMAKE_PREFIX_PATH=$qt;$ffmpeg;$stream" `
        "-DPYTHON_EXECUTABLE=$deps\venv\Scripts\python.exe" "-DPython3_EXECUTABLE=$deps\venv\Scripts\python.exe" "-DPython_EXECUTABLE=$deps\venv\Scripts\python.exe" `
        -DVEYRA_BUILD_QML_UI=ON -DVEYRA_ENABLE_EXPERIMENTAL_DLSSNR=ON -DVEYRA_ENABLE_D3D12_DEBUG=OFF `
        -DVEYRA_ENABLE_REMOTEPLAY=ON -DVEYRA_ENABLE_MOONLIGHT=ON -DVEYRA_ENABLE_XBOX=ON `
        "-DVEYRA_RP_CHIAKI_SOURCE_DIR=$deps\chiaki-stage" "-DVEYRA_RP_CHIAKI_VERIFY_DIR=$deps\chiaki-clean" `
        "-DVEYRA_MOONLIGHT_SOURCE_DIR=$deps\moonlight-common-c" `
        "-DPROTOC=$stream\tools\protobuf\protoc.exe" "-DPKG_CONFIG_EXECUTABLE=$stream\tools\pkgconf\pkgconf.exe" `
        -DOPENSSL_USE_STATIC_LIBS=ON -DOPENSSL_MSVC_STATIC_RT=ON `
        "-DVEYRA_DISPLAY_VERSION=3.2.3-lab" "-DVEYRA_FFMPEG_ROOT=$ffmpeg" `
        "-DVEYRA_DLSS_SDK_ROOT=$deps\dlss" "-DVEYRA_NVENC_HEADERS_ROOT=$deps\nv-codec-headers" `
        "-DVEYRA_CUDA_DRIVER_INCLUDE_DIR=$deps\cudart-root\include" `
        "-DVEYRA_XESS_ROOT=$($roots.XESS_ROOT)" "-DVEYRA_FIDELITYFX_ROOT=$($roots.FFX1_ROOT)" `
        "-DVEYRA_FIDELITYFX2_ROOT=$deps\ffx2\Kits\FidelityFX" "-DVEYRA_AMF_SDK_ROOT=$deps\amf\amf\public"
    Check
}
$t0 = Get-Date
cmake --build $out --target veyra_qml_ui --parallel; Check
"built in {0:N0} s: {1}" -f ((Get-Date) - $t0).TotalSeconds, (Get-Item "$out\PushFPS.exe").LastWriteTime
if ($Install) {
    $lab = 'F:\Games\PS5\PushFPS-lab'
    if (Get-Process PushFPS -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$lab*" }) { throw 'lab PushFPS is running' }
    if (-not (Test-Path "$lab\.prev\PushFPS.exe")) { New-Item -ItemType Directory -Force "$lab\.prev" | Out-Null; Copy-Item "$lab\PushFPS.exe" "$lab\.prev\" }
    Copy-Item "$out\PushFPS.exe" $lab -Force
    robocopy "$src\qml\Veyra" "$lab\qml\Veyra" /E /R:1 /W:1 /NFL /NDL /NJH /NJS /NP | Out-Null
    "installed into $lab"
}
