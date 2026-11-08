param([string]$OutputDirectory = 'artifacts/worker-dispatch/queues')
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = Join-Path $repositoryRoot $OutputDirectory
if (Test-Path $root) { throw 'Choose a new output directory' }
New-Item -ItemType Directory "$root/bin" -Force | Out-Null
Copy-Item "$repositoryRoot/Source/x64/Release/AreaUpdateExample.exe" "$root/bin/AreaUpdateExample.exe"
Get-ChildItem "$root/bin" -Filter *.exe | Get-FileHash | Export-Csv "$root/binaries.csv" -NoTypeInformation
$keys = @('AREAUPDATE_RADIUS_QUEUE_DIAGNOSTICS','AREAUPDATE_RADIUS_LATENCY_STRIDE','AREAUPDATE_RADIUS_SORT_TIMERS',
    'AREAUPDATE_RADIUS_PROFILE','AREAUPDATE_RADIUS_BATCH','AREAUPDATE_RADIUS_TIMER_BATCH',
    'AREAUPDATE_RADIUS_PUBLISH_BATCH','AREAUPDATE_RADIUS_FAST_BUCKET','AREAUPDATE_RADIUS_MERGE_NOTIFY',
    'AREAUPDATE_RADIUS_COALESCE_TIMERS')
$saved = @{}
foreach ($key in $keys) { $saved[$key] = [Environment]::GetEnvironmentVariable($key,'Process') }
$rows = @()
try {
    $env:AREAUPDATE_RADIUS_SORT_TIMERS='1'
    $env:AREAUPDATE_RADIUS_PROFILE='0'
    $env:AREAUPDATE_RADIUS_BATCH='64'
    $env:AREAUPDATE_RADIUS_TIMER_BATCH='1'
    $env:AREAUPDATE_RADIUS_PUBLISH_BATCH='1'
    $env:AREAUPDATE_RADIUS_FAST_BUCKET='1'
    $env:AREAUPDATE_RADIUS_MERGE_NOTIFY='1'
    $env:AREAUPDATE_RADIUS_COALESCE_TIMERS='0'
    foreach ($seed in @(42,43)) {
        $order = if ($seed -eq 42) { @(0,1) } else { @(1,0) }
        foreach ($diagnostics in $order) {
            $env:AREAUPDATE_RADIUS_QUEUE_DIAGNOSTICS="$diagnostics"
            $env:AREAUPDATE_RADIUS_LATENCY_STRIDE = if ($diagnostics) { '4096' } else { '0' }
            $name="queues-$diagnostics-$seed"
            & "$repositoryRoot/tools/benchmark/Measure-RuntimeCpu.ps1" -Seconds 20 -Actors 280000 -MapSide 640 -MovesPerSecond 8000 `
                -Seed $seed -JoinDurationMs 1000 -JoinIntervalMs 1 -Mode legacy -TimerTraceThresholdMs 20 -AllowStarvation `
                -ExecutablePath "$OutputDirectory/bin/AreaUpdateExample.exe" -OutputDirectory "$OutputDirectory/$name" | Out-Null
            $row = Get-Content "$root/$name/summary.json" -Raw | ConvertFrom-Json
            if ($row.processExitCode -ne 0 -or $row.tasksLive -ne 0 -or $row.neverUpdated -ne 0 -or
                $row.taskStatisticsEnabled -ne $true) { throw "Dispatch invariant: $name" }
            $row | Add-Member caseName $name
            $totals = @{}
            foreach ($line in (Get-Content "$root/$name/runtime.log")) {
                if ($line -match '^(WORKER_PHASE|RADIUS_QUEUE|RADIUS_PHASE) ') {
                    $prefix=$Matches[1]
                    foreach ($match in [regex]::Matches($line,'(\w+)=([^ ]+)')) {
                        $key="${prefix}_$($match.Groups[1].Value)"
                        if ($prefix -eq 'RADIUS_PHASE') { $totals[$key]=[double]$match.Groups[2].Value }
                        else { $totals[$key]+=[double]$match.Groups[2].Value }
                    }
                }
            }
            if ($diagnostics -eq 1 -and ($totals.RADIUS_PHASE_hol_samples -le 0 -or
                $totals.RADIUS_PHASE_samples -ne $totals.RADIUS_PHASE_hol_samples)) { throw 'Incomplete HOL coverage' }
            foreach ($key in $totals.Keys) { $row | Add-Member $key $totals[$key] }
            $rows += $row
            $columns = @($rows | ForEach-Object { $_.PSObject.Properties.Name } | Select-Object -Unique)
            $rows | Select-Object $columns | Export-Csv "$root/results.csv" -NoTypeInformation
            "$name FPS=$($row.steadyActorFps) timer_ns=$($totals.WORKER_PHASE_timer_ns) request_ns=$($totals.WORKER_PHASE_request_ns) ticker_ns=$($totals.WORKER_PHASE_ticker_ns) HOL_ms=$($totals.RADIUS_PHASE_hol_before_probe_ms)"
        }
    }
} finally {
    foreach ($key in $saved.Keys) { [Environment]::SetEnvironmentVariable($key,$saved[$key],'Process') }
}
