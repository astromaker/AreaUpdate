param([string]$OutputDirectory = 'artifacts/actor-deadline-followup/timer-order')
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = Join-Path $repositoryRoot $OutputDirectory
if (Test-Path $root) { throw 'Choose a new output directory' }
New-Item -ItemType Directory "$root/bin" -Force | Out-Null
foreach ($name in @('UpdateComparison','AreaUpdateExample')) {
    Copy-Item "$repositoryRoot/Source/x64/Release/$name.exe" "$root/bin/$name.exe"
}
$keys = @('AREAUPDATE_RADIUS_SORT_TIMERS','AREAUPDATE_RADIUS_LATENCY_STRIDE','AREAUPDATE_RADIUS_PROFILE',
    'AREAUPDATE_RADIUS_BATCH','AREAUPDATE_RADIUS_TIMER_BATCH','AREAUPDATE_RADIUS_PUBLISH_BATCH',
    'AREAUPDATE_RADIUS_FAST_BUCKET','AREAUPDATE_RADIUS_MERGE_NOTIFY')
$saved = @{}
foreach ($key in $keys) { $saved[$key] = [Environment]::GetEnvironmentVariable($key,'Process') }
$rows = @()
try {
    $env:AREAUPDATE_RADIUS_LATENCY_STRIDE='0'
    $env:AREAUPDATE_RADIUS_PROFILE='0'
    $env:AREAUPDATE_RADIUS_BATCH='64'
    $env:AREAUPDATE_RADIUS_TIMER_BATCH='1'
    $env:AREAUPDATE_RADIUS_PUBLISH_BATCH='1'
    $env:AREAUPDATE_RADIUS_FAST_BUCKET='1'
    $env:AREAUPDATE_RADIUS_MERGE_NOTIFY='1'
    foreach ($sort in @(0,1)) {
        $env:AREAUPDATE_RADIUS_SORT_TIMERS="$sort"
        foreach ($layout in @('uniform','boundary')) {
            $name = "verify-$sort-$layout"
            & "$root/bin/UpdateComparison.exe" --motion-probe legacy 5 5000 42 verify $layout none 64 0 0 0 *> "$root/$name.log"
            if ($LASTEXITCODE -ne 0) { throw "Order validation failed: $name" }
        }
    }
    foreach ($seed in @(42,43)) {
        $order = if ($seed -eq 42) { @('frozen','off','on') } else { @('on','off','frozen') }
        foreach ($kind in $order) {
            $env:AREAUPDATE_RADIUS_SORT_TIMERS = if ($kind -eq 'on') { '1' } else { '0' }
            $exe = if ($kind -eq 'frozen') { 'artifacts/actor-deadline-followup/capacity/bin/AreaUpdateExample.exe' }
                else { "$OutputDirectory/bin/AreaUpdateExample.exe" }
            $name = "$kind-$seed"
            & "$repositoryRoot/tools/benchmark/Measure-RuntimeCpu.ps1" -Seconds 20 -Actors 280000 -MapSide 640 -MovesPerSecond 8000 `
                -Seed $seed -JoinDurationMs 1000 -JoinIntervalMs 1 -Mode legacy -TimerTraceThresholdMs 20 -AllowStarvation `
                -ExecutablePath $exe -OutputDirectory "$OutputDirectory/$name" | Out-Null
            $row = Get-Content "$root/$name/summary.json" -Raw | ConvertFrom-Json
            if ($row.processExitCode -ne 0 -or $row.tasksLive -ne 0 -or $row.neverUpdated -ne 0 -or
                $row.taskStatisticsEnabled -ne $true) { throw "Order runtime invariant: $name" }
            $row | Add-Member caseName $name
            $timer = Get-Content "$root/$name/runtime.log" | Where-Object { $_ -like 'RADIUS_TIMERS *' } | Select-Object -Last 1
            foreach ($match in [regex]::Matches($timer,'(\w+)=([^ ]+)')) {
                $row | Add-Member "timer_$($match.Groups[1].Value)" $match.Groups[2].Value
            }
            $rows += $row
            $columns = @($rows | ForEach-Object { $_.PSObject.Properties.Name } | Select-Object -Unique)
            $rows | Select-Object $columns | Export-Csv "$root/results.csv" -NoTypeInformation
            "$name FPS=$($row.steadyActorFps) CPU=$($row.steadyCpuPercent) tasks=$($row.timer_tasks) reservations=$($row.timer_reservations)"
        }
    }
} finally {
    foreach ($key in $saved.Keys) { [Environment]::SetEnvironmentVariable($key,$saved[$key],'Process') }
}
