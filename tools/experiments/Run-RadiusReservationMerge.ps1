param(
    [string]$OutputDirectory = 'artifacts/worker-dispatch/reservation-merge',
    [string]$BinaryDirectory = 'artifacts/worker-dispatch/reservation-merge/bin'
)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference='Stop'
$root=Join-Path $repositoryRoot $OutputDirectory
if (Test-Path $root) { throw 'Choose a new output directory' }
New-Item -ItemType Directory "$root/bin" -Force | Out-Null
foreach ($name in @('AreaUpdateExample','UpdateComparison')) {
    Copy-Item "$repositoryRoot/$BinaryDirectory/$name.exe" "$root/bin/$name.exe"
}
$keys=@('AREAUPDATE_RADIUS_COALESCE_TIMERS','AREAUPDATE_RADIUS_QUEUE_DIAGNOSTICS','AREAUPDATE_RADIUS_LATENCY_STRIDE',
    'AREAUPDATE_RADIUS_SORT_TIMERS','AREAUPDATE_RADIUS_PROFILE','AREAUPDATE_RADIUS_BATCH','AREAUPDATE_RADIUS_TIMER_BATCH',
    'AREAUPDATE_RADIUS_PUBLISH_BATCH','AREAUPDATE_RADIUS_FAST_BUCKET','AREAUPDATE_RADIUS_MERGE_NOTIFY')
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
    foreach ($coalesce in @(0,1)) {
        $env:AREAUPDATE_RADIUS_COALESCE_TIMERS="$coalesce"
        foreach ($layout in @('uniform','boundary')) {
            & "$root/bin/UpdateComparison.exe" --motion-probe legacy 5 5000 42 verify $layout none 64 0 0 0 *> "$root/verify-$coalesce-$layout.log"
            if ($LASTEXITCODE -ne 0) { throw 'Coalescing ID validation failed' }
        }
    }
    $cases=@()
    foreach ($seed in @(42,43)) {
        $order=if ($seed -eq 42) { @(0,1) } else { @(1,0) }
        foreach ($coalesce in $order) { $cases+=@{seed=$seed;coalesce=$coalesce;actors=280000} }
    }
    foreach ($coalesce in @(0,1)) { $cases+=@{seed=42;coalesce=$coalesce;actors=70000} }
    foreach ($case in $cases) {
        $env:AREAUPDATE_RADIUS_COALESCE_TIMERS="$($case.coalesce)"
        $name="$($case.actors)-$($case.seed)-$($case.coalesce)"
        & "$repositoryRoot/tools/benchmark/Measure-RuntimeCpu.ps1" -Seconds 20 -Actors $case.actors -MapSide 640 -MovesPerSecond 8000 `
            -Seed $case.seed -JoinDurationMs 1000 -JoinIntervalMs 1 -Mode legacy -TimerTraceThresholdMs 20 -AllowStarvation `
            -ExecutablePath "$OutputDirectory/bin/AreaUpdateExample.exe" -OutputDirectory "$OutputDirectory/$name" | Out-Null
        $row=Get-Content "$root/$name/summary.json" -Raw | ConvertFrom-Json
        if ($row.processExitCode -ne 0 -or $row.tasksLive -ne 0 -or $row.neverUpdated -ne 0 -or
            $row.taskStatisticsEnabled -ne $true) { throw "Coalescing invariant: $name" }
        $row | Add-Member caseName $name
        $timer=Get-Content "$root/$name/runtime.log" | Where-Object { $_ -like 'RADIUS_TIMERS *' } | Select-Object -Last 1
        foreach ($match in [regex]::Matches($timer,'(\w+)=([^ ]+)')) {
            $row | Add-Member "timer_$($match.Groups[1].Value)" $match.Groups[2].Value
        }
        if ($case.coalesce -eq 1 -and [uint64]$row.timer_cross_batch_areas -eq 0) { throw 'Cross-batch path not exercised' }
        $rows+=$row
        $rows | Export-Csv "$root/results.csv" -NoTypeInformation
        "$name FPS=$($row.steadyActorFps) CPU=$($row.steadyCpuPercent) gap=$($row.maxGapMs) tasks=$($row.timer_tasks) merged=$($row.timer_cross_batch_areas)"
    }
} finally {
    foreach ($key in $saved.Keys) { [Environment]::SetEnvironmentVariable($key,$saved[$key],'Process') }
}
