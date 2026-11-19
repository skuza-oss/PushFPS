<#
  One unattended PushFPS lab trial against the PS5 (see F:\Dev\PushFPS\qml\Veyra\PushLab.qml).

  Applies a configuration to the lab copy (F:\Games\PS5\PushFPS-lab), starts it with the lab file, waits until it
  has connected, measured and quit, then cuts the engine statistics out of its log and writes
  results\<Name>.json plus one line in results\summary.csv. The stream settings that the lab shares with the
  user's own PushFPS/Veyra (PS5 profile fps, decode mode) are put back afterwards.

  Example:
    .\Run-Trial.ps1 -Name base30 -Fps 30
    .\Run-Trial.ps1 -Name sw60 -Fps 60 -DecodeMode 1 -Pref @{contentRate=1}
#>
param(
    [Parameter(Mandatory)][string]$Name,
    [int]$Fps = 30,
    [ValidateRange(0,2)][int]$DecodeMode = 2,
    [hashtable]$Pref = @{},
    # Line-3 token edits of the effects session: @{2=1; 10=2} (0 srTarget, 1 quality, 2 fgBackend, 3 flow, 10 fgMotion)
    [hashtable]$Session = @{},
    [int]$WarmupMs = 10000,
    [int]$StatsMs = 15000,
    [int]$LatencyTries = 12,
    [int]$QualitySeconds = 8,
    [int]$TimeoutSec = 300,
    [switch]$FrameLog,
    # 0 = leave as saved; 1 = frame generation off; 2..4 = multiplier
    [int]$FgMultiplier = 0,
    # extra environment variables for PushFPS (e.g. @{PUSHFPS_PACKET_DEDUP_BIAS='0.8'})
    [hashtable]$Env = @{}
)
$ErrorActionPreference = 'Stop'
$lab = 'F:\Games\PS5\PushFPS-lab'
$data = "$lab\runtime_local\user-data-2.0.0"
$rp = "$env:LOCALAPPDATA\Veyra\remoteplay"
$ini = "$rp\settings.ini"
$profileTool = 'F:\Temp\veyra-tools\profile.ps1'
$results = 'F:\Games\PS5\pushfps-lab\results'
$log = "$lab\logs\veyra-qml.log"

if (Get-Process PushFPS, veyra_qml_ui -ErrorAction SilentlyContinue) {
    throw 'PushFPS or Veyra is running. The PS5 has one Remote Play slot: close it first.'
}

# ---- apply configuration (remember the shared stream settings) ----
$dat = (Get-ChildItem "$rp\*.dat" | Select-Object -First 1).FullName
$profileBefore = & $profileTool -Path $dat
$fpsBefore = [int]([regex]::Match($profileBefore, 'fps=(\d+)').Groups[1].Value)
$iniBefore = [IO.File]::ReadAllText($ini)
& $profileTool -Path $dat -Fps $Fps -Write | Out-Null
[IO.File]::WriteAllText($ini, ($iniBefore -replace '(?m)^DecodeMode=\d', "DecodeMode=$DecodeMode"))

# Every trial starts from the same lab settings: snapshot now, put back after the run.
$snapshot = "$data.trial-snapshot"
if (Test-Path $snapshot) { Remove-Item $data -Recurse -Force; Move-Item $snapshot $data }   # an interrupted earlier trial
Copy-Item $data $snapshot -Recurse

$prefPath = "$data\qml-preferences.v1.json"
$prefs = [IO.File]::ReadAllText($prefPath) | ConvertFrom-Json
foreach ($k in $Pref.Keys) {
    $parts = $k.Split('.')
    $obj = $prefs
    for ($i = 0; $i -lt $parts.Count - 1; $i++) { $obj = $obj.($parts[$i]) }
    $obj | Add-Member -NotePropertyName $parts[-1] -NotePropertyValue $Pref[$k] -Force
}
[IO.File]::WriteAllText($prefPath, ($prefs | ConvertTo-Json -Depth 10), (New-Object Text.UTF8Encoding($false)))

$sessionFile = @("$data\chain-session.v1.effects.output-controls", "$data\chain-session.v1.effects.video-ai") | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($Session.Count) {
    $lines = [IO.File]::ReadAllText($sessionFile) -split "`n"
    $tok = $lines[2].Split(' ')
    foreach ($k in $Session.Keys) { $tok[[int]$k] = [string]$Session[$k] }
    $lines[2] = $tok -join ' '
    # keep the embedded preset's frame generation backend in step with token 2
    $text = $lines -join "`n"
    if ($Session.ContainsKey(2)) { $text = [regex]::Replace($text, '(\\"list\\" \\"\\" 0 0 15 \d+ )\d', "`${1}$($Session[2])") }
    [IO.File]::WriteAllText($sessionFile, $text)
}
if ($FgMultiplier -gt 0) {
    # list entry: name note kind builtin contents MULT backend audioSync audioOffset nodeCount mode CHAINMULT ...
    $text = [IO.File]::ReadAllText($sessionFile)
    $text = [regex]::Replace($text, '(\\"list\\" \\"\\" 0 0 \d+ )\d+( \d+ \d+ -?\d+ \d+ \d+ )\d+', "`${1}$FgMultiplier`${2}$FgMultiplier")
    [IO.File]::WriteAllText($sessionFile, $text)
}
$sessionLine = ((Get-Content $sessionFile -TotalCount 3)[2] -split ' ')[0..12] -join ' '

# ---- run ----
$cfg = @{ run = $Name; warmupMs = $WarmupMs; statsMs = $StatsMs; latencyTries = $LatencyTries; qualitySeconds = $QualitySeconds }
[IO.File]::WriteAllText("$lab\pushlab.json", ($cfg | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))
$startBytes = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }
$env:QML_XHR_ALLOW_FILE_READ = '1'
$env:VEYRA_VERBOSE_FRAME_LOGS = if ($FrameLog) { '1' } else { '' }
foreach ($k in @('PUSHFPS_PACKET_DEDUP', 'PUSHFPS_PACKET_DEDUP_SHARE', 'PUSHFPS_PACKET_DEDUP_BIAS')) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
foreach ($k in $Env.Keys) { Set-Item "env:$k" ([string]$Env[$k]) }
$proc = Start-Process "$lab\PushFPS.exe" -WorkingDirectory $lab -PassThru
$timedOut = -not $proc.WaitForExit($TimeoutSec * 1000)
if ($timedOut) { Stop-Process -Id $proc.Id -Force; Start-Sleep 2 }
Remove-Item "$lab\pushlab.json" -Force -ErrorAction SilentlyContinue

# ---- restore shared stream settings ----
& $profileTool -Path $dat -Fps $fpsBefore -Write | Out-Null
[IO.File]::WriteAllText($ini, $iniBefore)
Remove-Item $data -Recurse -Force; Move-Item $snapshot $data

# ---- read this run's log ----
$s = [IO.File]::Open($log, 'Open', 'Read', 'ReadWrite'); $s.Seek($startBytes, 'Begin') | Out-Null
$r = New-Object IO.StreamReader($s); $text = $r.ReadToEnd(); $r.Close()
$lines = $text -split "`n"
function Stamp($line) { [datetimeoffset]::Parse($line.Substring(0, 24)) }
$phaseLines = $lines | Where-Object { $_ -match '\[push-lab\] phase=' }
$statsBegin = $phaseLines | Where-Object { $_ -match 'phase=stats' } | Select-Object -First 1
$statsEnd = $phaseLines | Where-Object { $_ -match 'phase=(latency|quality-start)' } | Select-Object -First 1
$metrics = [ordered]@{}
if ($statsBegin -and $statsEnd) {
    $t0 = Stamp $statsBegin; $t1 = Stamp $statsEnd
    $window = $lines | Where-Object { $_.Length -gt 24 -and $_[4] -eq '-' } | Where-Object { $t = Stamp $_; $t -ge $t0 -and $t -le $t1 }
    $collect = @{
        'capture-timing' = 'callbackToPresentReturnP95Ms', 'gpuReadyP95Ms', 'gpuSrP95Ms', 'gpuFlowP95Ms', 'presentCpuP95Ms'
        'remoteplay-progress' = 'inputFps', 'decodeFps', 'videoMbps', 'receiveAgeMs', 'decodeAgeMs'
        'frame-rate' = 'gpuCompletedFps', 'presentSubmitFps'
        'content-dedup' = 'kept', 'repeats', 'blind', 'contentFps'
    }
    foreach ($tag in $collect.Keys) {
        $tagLines = $window | Where-Object { $_ -match "\[$tag\]" }
        foreach ($field in $collect[$tag]) {
            $vals = @($tagLines | ForEach-Object { if ($_ -match "\b$field=(-?[\d.]+)") { [double]$matches[1] } })
            if ($vals.Count) {
                $sorted = $vals | Sort-Object
                $metrics["$field"] = [ordered]@{ mean = [math]::Round(($vals | Measure-Object -Average).Average, 2); median = $sorted[[int][math]::Floor(($sorted.Count - 1) / 2)]; max = $sorted[-1]; n = $vals.Count }
            }
        }
    }
    $metrics['fgValidityLines'] = @($window | Where-Object { $_ -match '\[fg-validity\]' }).Count
    $xg = @($window | Where-Object { $_ -match '\[xess-fg\] presentId' } | ForEach-Object { if ($_ -match 'generatedTotal=(\d+)') { [double]$matches[1] } })
    if ($xg.Count -ge 2) { $metrics['xessGeneratedPerSec'] = [math]::Round(($xg[-1] - $xg[0]) / (((Stamp ($window | Where-Object { $_ -match '\[xess-fg\] presentId' } | Select-Object -Last 1)) - (Stamp ($window | Where-Object { $_ -match '\[xess-fg\] presentId' } | Select-Object -First 1))).TotalSeconds), 1) }
}
if ($FrameLog) {
    $frames = foreach ($l in $lines) {
        if ($l -match 'content-dedup-frame\] source=(\d+) pts100ns=(\d+) keep=(\w+) reason=(\S+) picture=(\S+) withinBand=(\w+) over=(\d+) maxDiff=(\d+)') {
            $o = [ordered]@{ src = $matches[1]; pts = $matches[2]; keep = $matches[3]; reason = $matches[4]; picture = $matches[5]; band = $matches[6]; over = $matches[7]; max = $matches[8]; bytes = ''; packetRepeat = ''; threshold = '' }
            if ($l -match 'bytes=(\d+) packetRepeat=(\w+) packetThreshold=(\d+)') { $o.bytes = $matches[1]; $o.packetRepeat = $matches[2]; $o.threshold = $matches[3] }
            [pscustomobject]$o
        }
    }
    $frames | Export-Csv "$results\$Name-frames.csv" -NoTypeInformation
}
function LabJson($prefix) { $l = $lines | Where-Object { $_ -match "\[push-lab\] $prefix " } | Select-Object -Last 1; if ($l) { $l.Substring($l.IndexOf("] $prefix ") + $prefix.Length + 3) | ConvertFrom-Json } }
$latency = LabJson 'latency'; $quality = LabJson 'quality'; $done = LabJson 'done'
$warn = $lines | Where-Object { $_ -match '\[(WARN |ERROR)\] \[([\w-]+)\]' } | ForEach-Object { $matches[2] } | Group-Object | ForEach-Object { "$($_.Name)x$($_.Count)" }

$result = [ordered]@{
    name = $Name; at = (Get-Date).ToString('s'); timedOut = $timedOut
    config = [ordered]@{ fps = $Fps; decodeMode = $DecodeMode; pref = $Pref; session = $sessionLine }
    ok = $done.ok; reason = $done.reason
    metrics = $metrics
    latency = $latency; quality = $quality; warnings = @($warn)
}
$json = $result | ConvertTo-Json -Depth 8
[IO.File]::WriteAllText("$results\$Name.json", $json, (New-Object Text.UTF8Encoding($false)))
$m = $metrics
$row = [pscustomobject]@{
    name = $Name; at = $result.at; ok = $done.ok; fps = $Fps; decode = $DecodeMode; session = $sessionLine
    procP95 = $m['callbackToPresentReturnP95Ms'].median; gpuP95 = $m['gpuReadyP95Ms'].median
    keptPerSec = $m['kept'].median; repeatsPerSec = $m['repeats'].median; blindPerSec = $m['blind'].median
    fgLines = $m['fgValidityLines']; xessGenPerSec = $m['xessGeneratedPerSec']
    latMedian = $latency.medianMs; latMin = $latency.minMs; latMax = $latency.maxMs; answered = "$($latency.answered)/$($latency.trials)"
    psnr = $quality.psnr; gainOverBlend = $quality.gainOverBlend; onTime = $quality.onTime; qScore = $quality.score
}
$row | Export-Csv "$results\summary.csv" -Append -NoTypeInformation -Encoding UTF8
$row | Format-List
if (-not $done) { Write-Warning "No 'done' line: the lab did not finish (timedOut=$timedOut). Check $log" }
