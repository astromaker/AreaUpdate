param(
    [ValidateRange(10,120)][int]$Seconds = 20,
    [string]$OutputDirectory = 'artifacts/actor-deadline-followup/queue-profile',
    [string]$BaselineExecutable = 'artifacts/actor-deadline-followup/capacity/bin/AreaUpdateExample.exe',
    [string]$PreviousExecutable = 'artifacts/radius-latency-v3/capacity/bin/AreaUpdateExample.exe'
)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = Join-Path $repositoryRoot $OutputDirectory
if (Test-Path $root) { throw 'Choose a new output directory' }
New-Item -ItemType Directory "$root/bin" -Force | Out-Null
Copy-Item "$repositoryRoot/Source/x64/Release/AreaUpdateExample.exe" "$root/bin/AreaUpdateExample.exe"
Copy-Item "$repositoryRoot/Source/x64/Release/UpdateComparison.exe" "$root/bin/UpdateComparison.exe"
Get-ChildItem "$root/bin" -File | Get-FileHash | Export-Csv "$root/binaries.csv" -NoTypeInformation
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
    $env:AREAUPDATE_RADIUS_SORT_TIMERS='0'
    $cases = @()
    foreach ($seed in @(42,43)) {
        $order = if ($seed -eq 42) { @('previous','baseline','off','sampled') } else { @('sampled','off','baseline','previous') }
        foreach ($kind in $order) {
            $cases += @{name="$kind-$seed";kind=$kind;seed=$seed;actors=280000;side=640;moves=8000;stride=4096}
        }
    }
    $cases += @{name='stationary-42';kind='sampled';seed=42;actors=280000;side=640;moves=0;stride=4096}
    $cases += @{name='lowload-42';kind='sampled';seed=42;actors=5000;side=64;moves=2000;stride=16}
    foreach ($case in $cases) {
        $env:AREAUPDATE_RADIUS_LATENCY_STRIDE = if ($case.kind -eq 'sampled') { "$($case.stride)" } else { '0' }
        $exe = switch ($case.kind) {
            'previous' { $PreviousExecutable }
            'baseline' { $BaselineExecutable }
            default { "$OutputDirectory/bin/AreaUpdateExample.exe" }
        }
        & "$repositoryRoot/tools/benchmark/Measure-RuntimeCpu.ps1" -Seconds $Seconds -Actors $case.actors -MapSide $case.side `
            -MovesPerSecond $case.moves -Seed $case.seed -JoinDurationMs 1000 -JoinIntervalMs 1 -Mode legacy `
            -TimerTraceThresholdMs 20 -AllowStarvation -ExecutablePath $exe -OutputDirectory "$OutputDirectory/$($case.name)" | Out-Null
        $row = Get-Content "$root/$($case.name)/summary.json" -Raw | ConvertFrom-Json
        if ($row.processExitCode -ne 0 -or $row.neverUpdated -ne 0 -or $row.tasksLive -ne 0 -or
            $row.taskStatisticsEnabled -ne $true) { throw "Runtime invariant: $($case.name)" }
        $row | Add-Member caseName $case.name
        if ($case.kind -eq 'sampled') {
            $text = Get-Content "$root/$($case.name)/runtime.log"
            foreach ($prefix in @('RADIUS_LATENCY','RADIUS_PHASE')) {
                $line = $text | Where-Object { $_ -like "$prefix *" } | Select-Object -Last 1
                if (-not $line) { throw "Missing $prefix" }
                foreach ($match in [regex]::Matches($line,'(\w+)=([^ ]+)')) {
                    $row | Add-Member "$($prefix)_$($match.Groups[1].Value)" $match.Groups[2].Value
                }
            }
            if ([uint64]$row.RADIUS_PHASE_samples -eq 0 -or
                $row.RADIUS_PHASE_samples -ne $row.RADIUS_LATENCY_samples) { throw 'Incomplete phase samples' }
            $sum = [double]$row.RADIUS_PHASE_publish_ms + [double]$row.RADIUS_PHASE_dispatch_ms +
                [double]$row.RADIUS_PHASE_retry_ms + [double]$row.RADIUS_PHASE_batch_ms
            if ([Math]::Abs($sum-[double]$row.RADIUS_LATENCY_mean_area_wait_ms) -gt 0.002) {
                throw 'Phase decomposition does not equal the total wait'
            }
        }
        $rows += $row
        # The first control row has no phase properties; explicitly keep the union of columns.
        $columns = @($rows | ForEach-Object { $_.PSObject.Properties.Name } | Select-Object -Unique)
        $rows | Select-Object $columns | Export-Csv "$root/results.csv" -NoTypeInformation
        "$($case.name) FPS=$($row.steadyActorFps) maxGap=$($row.maxGapMs)"
    }
    $env:AREAUPDATE_RADIUS_LATENCY_STRIDE='0'
    foreach ($layout in @('uniform','boundary')) {
        & "$root/bin/UpdateComparison.exe" --motion-probe legacy 30 5000 42 verify $layout none 64 0 90 10 *> "$root/actor-$layout.log"
        if ($LASTEXITCODE -ne 0) { throw "Actor trace failed: $layout" }
        Get-Content "$root/actor-$layout.log" | Where-Object { $_ -like 'MOTION *' -or $_ -like 'TRACE_SUMMARY *' }
    }
} finally {
    foreach ($key in $saved.Keys) { [Environment]::SetEnvironmentVariable($key,$saved[$key],'Process') }
}
