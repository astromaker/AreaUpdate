param(
    [string]$OutputDirectory = 'artifacts/worker-dispatch/budget',
    [string]$ExecutablePath = 'artifacts/worker-dispatch/budget/bin/AreaUpdateExample.exe'
)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = Join-Path $repositoryRoot $OutputDirectory
if (Test-Path $root) { throw 'Choose a new output directory' }
New-Item -ItemType Directory "$root/bin" -Force | Out-Null
Copy-Item (Join-Path $repositoryRoot $ExecutablePath) "$root/bin/AreaUpdateExample.exe"
Get-ChildItem "$root/bin" -Filter *.exe | Get-FileHash | Export-Csv "$root/binaries.csv" -NoTypeInformation
$keys = @('AREAUPDATE_RADIUS_SERVICE_US','AREAUPDATE_RADIUS_QUEUE_DIAGNOSTICS','AREAUPDATE_RADIUS_LATENCY_STRIDE',
    'AREAUPDATE_RADIUS_SORT_TIMERS','AREAUPDATE_RADIUS_PROFILE','AREAUPDATE_RADIUS_BATCH',
    'AREAUPDATE_RADIUS_TIMER_BATCH','AREAUPDATE_RADIUS_PUBLISH_BATCH','AREAUPDATE_RADIUS_FAST_BUCKET',
    'AREAUPDATE_RADIUS_MERGE_NOTIFY')
$saved=@{}
foreach ($key in $keys) { $saved[$key]=[Environment]::GetEnvironmentVariable($key,'Process') }
$rows=@()
try {
    $env:AREAUPDATE_RADIUS_QUEUE_DIAGNOSTICS='0'
    $env:AREAUPDATE_RADIUS_LATENCY_STRIDE='0'
    $env:AREAUPDATE_RADIUS_SORT_TIMERS='1'
    $env:AREAUPDATE_RADIUS_PROFILE='0'
    $env:AREAUPDATE_RADIUS_BATCH='64'
    $env:AREAUPDATE_RADIUS_TIMER_BATCH='1'
    $env:AREAUPDATE_RADIUS_PUBLISH_BATCH='1'
    $env:AREAUPDATE_RADIUS_FAST_BUCKET='1'
    $env:AREAUPDATE_RADIUS_MERGE_NOTIFY='1'
    $cases=@()
    foreach ($seed in @(42,43)) {
        $order=if ($seed -eq 42) { @(0,250) } else { @(250,0) }
        foreach ($budget in $order) { $cases+=@{seed=$seed;budget=$budget;actors=280000} }
    }
    foreach ($budget in @(0,250)) { $cases+=@{seed=42;budget=$budget;actors=70000} }
    foreach ($case in $cases) {
        $env:AREAUPDATE_RADIUS_SERVICE_US="$($case.budget)"
        $name="$($case.actors)-$($case.seed)-$($case.budget)"
        & "$repositoryRoot/tools/benchmark/Measure-RuntimeCpu.ps1" -Seconds 20 -Actors $case.actors -MapSide 640 -MovesPerSecond 8000 `
            -Seed $case.seed -JoinDurationMs 1000 -JoinIntervalMs 1 -Mode legacy -TimerTraceThresholdMs 20 -AllowStarvation `
            -ExecutablePath "$OutputDirectory/bin/AreaUpdateExample.exe" -OutputDirectory "$OutputDirectory/$name" | Out-Null
        $row=Get-Content "$root/$name/summary.json" -Raw | ConvertFrom-Json
        if ($row.processExitCode -ne 0 -or $row.tasksLive -ne 0 -or $row.neverUpdated -ne 0 -or
            $row.taskStatisticsEnabled -ne $true) { throw "Service invariant: $name" }
        $row | Add-Member caseName $name
        $totals=@{}
        foreach ($line in (Get-Content "$root/$name/runtime.log" | Where-Object { $_ -like 'WORKER_SERVICE *' })) {
            foreach ($match in [regex]::Matches($line,'(\w+)=([^ ]+)')) {
                $totals[$match.Groups[1].Value]+=[uint64]$match.Groups[2].Value
            }
        }
        if (-not $totals.Count -or ($case.budget -gt 0 -and $totals.extra_calls -eq 0)) {
            throw 'Experimental service path was not exercised'
        }
        foreach ($key in $totals.Keys) { $row | Add-Member "service_$key" $totals[$key] }
        $rows+=$row
        $rows | Export-Csv "$root/results.csv" -NoTypeInformation
        "$name FPS=$($row.steadyActorFps) CPU=$($row.steadyCpuPercent) maxGap=$($row.maxGapMs) timer=$($row.maxTimerDelayMs) extra=$($totals.extra_calls)"
    }
} finally {
    foreach ($key in $saved.Keys) { [Environment]::SetEnvironmentVariable($key,$saved[$key],'Process') }
}
