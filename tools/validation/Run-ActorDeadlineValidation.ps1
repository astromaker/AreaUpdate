param(
    [ValidateSet('stability','capacity')][string]$Stage = 'stability',
    [ValidateRange(60,300)][int]$LongSeconds = 300,
    [ValidateSet(0,1)][int]$SortTimers = 1,
    [string]$OutputDirectory = 'artifacts/actor-deadline-followup'
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
Get-ChildItem "$root/bin" -Filter *.exe | Get-FileHash | Export-Csv "$root/binaries.csv" -NoTypeInformation
$keys = @('AREAUPDATE_RADIUS_PROFILE','AREAUPDATE_RADIUS_BATCH','AREAUPDATE_RADIUS_TIMER_BATCH',
    'AREAUPDATE_RADIUS_PUBLISH_BATCH','AREAUPDATE_RADIUS_FAST_BUCKET','AREAUPDATE_RADIUS_MERGE_NOTIFY',
    'AREAUPDATE_RADIUS_LATENCY_STRIDE','AREAUPDATE_RADIUS_SORT_TIMERS')
$saved = @{}
foreach ($key in $keys) { $saved[$key] = [Environment]::GetEnvironmentVariable($key,'Process') }
$rows = @()
try {
    $env:AREAUPDATE_RADIUS_PROFILE='0'
    $env:AREAUPDATE_RADIUS_BATCH='64'
    $env:AREAUPDATE_RADIUS_TIMER_BATCH='1'
    $env:AREAUPDATE_RADIUS_PUBLISH_BATCH='1'
    $env:AREAUPDATE_RADIUS_FAST_BUCKET='1'
    $env:AREAUPDATE_RADIUS_MERGE_NOTIFY='1'
    $env:AREAUPDATE_RADIUS_LATENCY_STRIDE='0'
    $env:AREAUPDATE_RADIUS_SORT_TIMERS="$SortTimers"
    if ($Stage -eq 'capacity') {
        foreach ($seed in @(42,43)) {
            foreach ($mode in @('legacy','area')) {
                $name = "$mode-280000-$seed"
                & "$repositoryRoot/tools/benchmark/Measure-RuntimeCpu.ps1" -Seconds 30 -Actors 280000 -MapSide 640 -MovesPerSecond 8000 `
                    -Seed $seed -JoinDurationMs 1000 -JoinIntervalMs 1 -Mode $mode -WaitMode signal -AllowStarvation `
                    -TimerTraceThresholdMs 20 -ExecutablePath "$OutputDirectory/$Stage/bin/AreaUpdateExample.exe" `
                    -OutputDirectory "$OutputDirectory/$Stage/$name" | Out-Null
                $row = Get-Content "$root/$name/summary.json" -Raw | ConvertFrom-Json
                if ($row.processExitCode -ne 0 -or $row.taskStatisticsEnabled -ne $true -or
                    $row.neverUpdated -ne 0 -or $row.tasksLive -ne 0) { throw "Capacity invariant: $name" }
                $row | Add-Member caseName $name
                $rows += $row
                $rows | Export-Csv "$root/results.csv" -NoTypeInformation
                "$name FPS=$($row.steadyActorFps) CPU=$($row.steadyCpuPercent) maxGap=$($row.maxGapMs)"
            }
        }
    } else {
        foreach ($workload in @('synthetic','verify')) {
            foreach ($mode in @('legacy','area')) {
                foreach ($layout in @('uniform','boundary')) {
                    $seconds = if ($workload -eq 'synthetic' -and $mode -eq 'legacy') { $LongSeconds } else { 60 }
                    $name = "$mode-$workload-$layout"
                    $directory = "$root/$name"
                    New-Item -ItemType Directory $directory | Out-Null
                    $watch = [Diagnostics.Stopwatch]::StartNew()
                    $process = Start-Process -FilePath "$root/bin/UpdateComparison.exe" -WindowStyle Hidden -PassThru `
                        -ArgumentList @('--motion-probe',$mode,$seconds,5000,42,$workload,$layout,'none',64,0,0,0) `
                        -RedirectStandardOutput "$directory/runtime.log" -RedirectStandardError "$directory/stderr.log"
                    $previousCpu = 0.0
                    $previousSeconds = 0.0
                    $progress = 30
                    while (-not $process.WaitForExit(1000)) {
                        $process.Refresh()
                        if ($process.HasExited) { break }
                        $elapsed = $watch.Elapsed.TotalSeconds
                        if ($elapsed -gt $seconds + 90) {
                            $process.Kill()
                            $process.WaitForExit()
                            throw "Watchdog: $name"
                        }
                        $cpu = $process.TotalProcessorTime.TotalSeconds
                        $sample = [pscustomobject]@{elapsed=$elapsed;privateMB=$process.PrivateMemorySize64/1MB;
                            cpuPercent=100*($cpu-$previousCpu)/($elapsed-$previousSeconds)/[Environment]::ProcessorCount}
                        $sample | Export-Csv "$directory/samples.csv" -NoTypeInformation -Append
                        $previousCpu = $cpu
                        $previousSeconds = $elapsed
                        if ($elapsed -ge $progress) {
                            '{0} elapsed={1:F0}s private={2:F2}MiB CPU={3:F2}%' -f $name,$elapsed,$sample.privateMB,$sample.cpuPercent
                            $progress += 30
                        }
                    }
                    $process.WaitForExit()
                    if ($process.ExitCode -ne 0) { throw "Motion process failed: $name" }
                    $line = Get-Content "$directory/runtime.log" | Where-Object { $_ -like 'MOTION *' } | Select-Object -Last 1
                    if (-not $line) { throw "Missing summary: $name" }
                    $row = [ordered]@{caseName=$name}
                    foreach ($match in [regex]::Matches($line,'(\w+)=([^ ]+)')) {
                        $row[$match.Groups[1].Value] = $match.Groups[2].Value
                    }
                    if ($row.sent -ne $row.applied -or $row.overlaps -ne '0' -or $row.tasks_live -ne '0' -or
                        [uint64]$row.gap_samples -eq 0 -or ($workload -eq 'verify' -and $row.ownership_cycle -ne '1')) {
                        throw "Motion invariant: $name"
                    }
                    $samples = Import-Csv "$directory/samples.csv"
                    $row.firstPrivateMB = ($samples | Where-Object { [double]$_.elapsed -ge 30 -and [double]$_.elapsed -lt 60 } |
                        Measure-Object privateMB -Average).Average
                    $row.lastPrivateMB = ($samples | Where-Object { [double]$_.elapsed -ge $seconds-30 } |
                        Measure-Object privateMB -Average).Average
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
