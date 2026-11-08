param(
    [ValidateSet('delay','motion','capacity')][string]$Stage = 'delay',
    [ValidateRange(60,300)][int]$LongSeconds = 300,
    [string]$OutputDirectory = 'artifacts/radius-stability'
)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = Join-Path $repositoryRoot "$OutputDirectory/$Stage"
if (Test-Path $root) { throw 'Choose a new output directory' }
New-Item -ItemType Directory "$root/bin" -Force | Out-Null
foreach ($name in @('UpdateComparison','AreaUpdateExample')) {
    Copy-Item "$repositoryRoot/Source/x64/Release/$name.exe" "$root/bin/$name.exe"
    Copy-Item "$repositoryRoot/Source/x64/Release/$name.pdb" "$root/bin/$name.pdb"
}
$keys = @('AREAUPDATE_RADIUS_PROFILE','AREAUPDATE_RADIUS_BATCH','AREAUPDATE_RADIUS_TIMER_BATCH','AREAUPDATE_RADIUS_PUBLISH_BATCH','AREAUPDATE_RADIUS_FAST_BUCKET','AREAUPDATE_RADIUS_MERGE_NOTIFY')
$saved = @{}
foreach ($key in $keys) { $saved[$key] = [Environment]::GetEnvironmentVariable($key, 'Process') }
$rows = @()
try {
    $env:AREAUPDATE_RADIUS_PROFILE = '0'
    $env:AREAUPDATE_RADIUS_BATCH = '64'
    $env:AREAUPDATE_RADIUS_TIMER_BATCH = '1'
    $env:AREAUPDATE_RADIUS_PUBLISH_BATCH = '1'
    $env:AREAUPDATE_RADIUS_FAST_BUCKET = '1'
    if ($Stage -eq 'delay') {
        foreach ($phase in @('timer','request','area')) {
            foreach ($load in @('sleep','busy')) {
                foreach ($stall in @(10,50,200)) {
                    foreach ($merge in @(0,1)) {
                        $name = "$phase-$load-$stall-$merge"
                        & "$root/bin/UpdateComparison.exe" --radius-delay-probe $merge $phase $stall 5 $load *> "$root/$name.log"
                        if ($LASTEXITCODE -ne 0) { throw "Delay probe failed: $name" }
                        $line = Get-Content "$root/$name.log" | Where-Object { $_ -match '^RADIUS_DELAY ' } | Select-Object -Last 1
                        if (-not $line) { throw 'Missing delay summary' }
                        $row = [ordered]@{caseName=$name}
                        foreach ($match in [regex]::Matches($line,'(\w+)=([^ ]+)')) { $row[$match.Groups[1].Value]=$match.Groups[2].Value }
                        if ($row.tasks_live -ne '0' -or ($phase -eq 'area' -and $row.early_updates -ne '0')) { throw 'Delay invariant failed' }
                        if ($phase -ne 'area' -and $stall -eq 200 -and [double]$row.median_ms -ge 100) { throw 'Independent work still waits for long callback' }
                        $rows += [pscustomobject]$row
                        $rows | Export-Csv "$root/results.csv" -NoTypeInformation
                        $line
                    }
                }
            }
        }
    } elseif ($Stage -eq 'capacity') {
        foreach ($seed in @(42,43)) {
            $modes = if ($seed -eq 42) { @(0,1) } else { @(1,0) }
            foreach ($merge in $modes) {
                $env:AREAUPDATE_RADIUS_MERGE_NOTIFY = "$merge"
                $name = "280000-$seed-merge$merge"
                & "$repositoryRoot/tools/benchmark/Measure-RuntimeCpu.ps1" -Seconds 30 -Actors 280000 -MapSide 640 -MovesPerSecond 8000 `
                    -Seed $seed -JoinDurationMs 1000 -JoinIntervalMs 1 -Mode legacy -WaitMode signal -AllowStarvation `
                    -ExecutablePath "$OutputDirectory/$Stage/bin/AreaUpdateExample.exe" -OutputDirectory "$OutputDirectory/$Stage/$name" | Out-Null
                $row = Get-Content "$root/$name/summary.json" -Raw | ConvertFrom-Json
                if ($row.taskStatisticsEnabled -ne $true -or $row.neverUpdated -ne 0) { throw 'Invalid capacity validation' }
                $row | Add-Member merge $merge
                $rows += $row
                $rows | Export-Csv "$root/results.csv" -NoTypeInformation
                "$name FPS=$($row.steadyActorFps) CPU=$($row.steadyCpuPercent) maxGap=$($row.maxGapMs)"
            }
        }
    } else {
        foreach ($workload in @('synthetic','verify')) {
            foreach ($layout in @('uniform','boundary')) {
                $modes = if ($layout -eq 'uniform') { @(0,1) } else { @(1,0) }
                foreach ($merge in $modes) {
                    $env:AREAUPDATE_RADIUS_MERGE_NOTIFY = "$merge"
                    $seconds = if ($workload -eq 'synthetic') { $LongSeconds } else { 60 }
                    $name = "$workload-$layout-merge$merge"
                    $directory = "$root/$name"
                    New-Item -ItemType Directory $directory | Out-Null
                    $watch = [Diagnostics.Stopwatch]::StartNew()
                    $process = Start-Process -FilePath "$root/bin/UpdateComparison.exe" -ArgumentList @('--motion-probe','legacy',$seconds,5000,42,$workload,$layout) -WindowStyle Hidden -PassThru -RedirectStandardOutput "$directory/runtime.log" -RedirectStandardError "$directory/stderr.log"
                    $samples = @()
                    $progress = 30
                    $previousCpu = 0.0
                    $previousSeconds = 0.0
                    while (-not $process.WaitForExit(1000)) {
                        $process.Refresh()
                        if ($process.HasExited) { break }
                        $elapsed = $watch.Elapsed.TotalSeconds
                        if ($elapsed -gt $seconds + 90) { $process.Kill(); $process.WaitForExit(); throw "Motion watchdog: $name" }
                        $cpu = $process.TotalProcessorTime.TotalSeconds
                        $sample = [pscustomobject]@{elapsed=$elapsed;privateMB=$process.PrivateMemorySize64 / 1MB;workingSetMB=$process.WorkingSet64 / 1MB;cpuPercent=100*($cpu-$previousCpu)/($elapsed-$previousSeconds)/[Environment]::ProcessorCount}
                        $samples += $sample
                        $sample | Export-Csv "$directory/samples.csv" -NoTypeInformation -Append
                        $previousCpu = $cpu
                        $previousSeconds = $elapsed
                        if ($elapsed -ge $progress) {
                            '{0} elapsed={1:F0}s private={2:F2}MiB CPU={3:F2}%' -f $name,$elapsed,$sample.privateMB,$sample.cpuPercent
                            $progress += 30
                        }
                    }
                    $process.WaitForExit()
                    if ($process.ExitCode -ne 0) { throw "Motion failed: $name" }
                    $line = Get-Content "$directory/runtime.log" | Where-Object { $_ -match '^MOTION ' } | Select-Object -Last 1
                    if (-not $line) { throw 'Missing motion summary' }
                    $row = [ordered]@{caseName=$name;merge=$merge}
                    foreach ($match in [regex]::Matches($line,'(\w+)=([^ ]+)')) { $row[$match.Groups[1].Value]=$match.Groups[2].Value }
                    if ($row.sent -ne $row.applied -or $row.overlaps -ne '0' -or $row.tasks_live -ne '0' -or [uint64]$row.gap_samples -eq 0) { throw 'Motion invariant failed' }
                    if ($workload -eq 'verify' -and $row.ownership_cycle -ne '1') { throw 'Missing ownership validation' }
                    $row.cpuPercent = 100*[double]$row.cpu_seconds/[double]$row.seconds/[Environment]::ProcessorCount
                    $row.peakPrivateMB = ($samples | Measure-Object privateMB -Maximum).Maximum
                    $row.firstWindowPrivateMB = ($samples | Where-Object { $_.elapsed -ge 30 -and $_.elapsed -lt 60 } | Measure-Object privateMB -Average).Average
                    $row.lastWindowPrivateMB = ($samples | Where-Object { $_.elapsed -ge $seconds - 30 -and $_.elapsed -lt $seconds } | Measure-Object privateMB -Average).Average
                    $rows += [pscustomobject]$row
                    $rows | Export-Csv "$root/results.csv" -NoTypeInformation
                    $line
                }
            }
        }
    }
} finally {
    foreach ($key in $saved.Keys) { [Environment]::SetEnvironmentVariable($key,$saved[$key],'Process') }
}
