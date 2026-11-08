param(
    [ValidateSet('scout','final','profile','boundary')][string]$Stage = 'scout',
    [ValidateRange(10,60)][int]$Seconds = 20,
    [string]$OutputDirectory = 'artifacts/radius-publication'
)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = Join-Path $repositoryRoot "$OutputDirectory/$Stage"
if (Test-Path $root) { throw 'Choose a new output directory' }
New-Item -ItemType Directory "$root/bin" -Force | Out-Null
Copy-Item "$repositoryRoot/Source/x64/Release/AreaUpdateExample.exe" "$root/bin/AreaUpdateExample.exe"
Copy-Item "$repositoryRoot/Source/x64/Release/AreaUpdateExample.pdb" "$root/bin/AreaUpdateExample.pdb"
$saved = @{}
foreach ($key in @('AREAUPDATE_RADIUS_PROFILE','AREAUPDATE_RADIUS_BATCH','AREAUPDATE_RADIUS_TIMER_BATCH','AREAUPDATE_RADIUS_PUBLISH_BATCH','AREAUPDATE_RADIUS_FAST_BUCKET','AREAUPDATE_RADIUS_MERGE_NOTIFY')) {
    $saved[$key] = [Environment]::GetEnvironmentVariable($key, 'Process')
}
$rows = @()
try {
    $env:AREAUPDATE_RADIUS_FAST_BUCKET = '0'
    $env:AREAUPDATE_RADIUS_MERGE_NOTIFY = '0'
    $actorsList = if ($Stage -eq 'final') { @(70000,140000,280000) } elseif ($Stage -eq 'boundary') { @(70000) } else { @(280000) }
    $seeds = if ($Stage -eq 'profile') { @(42) } else { @(42,43) }
    foreach ($actors in $actorsList) {
        $side = switch ($actors) { 70000 {320} 140000 {453} 280000 {640} }
        foreach ($seed in $seeds) {
            $variants = if ($Stage -eq 'scout') { @('area','single','timer','publish') } elseif ($Stage -eq 'profile') { @('timer','publish') } else { @('area','single','publish') }
            if ($seed -eq 43) { [array]::Reverse($variants) }
            foreach ($variant in $variants) {
                $env:AREAUPDATE_RADIUS_PROFILE = if ($Stage -eq 'profile') { '64' } else { '0' }
                $env:AREAUPDATE_RADIUS_BATCH = '64'
                $env:AREAUPDATE_RADIUS_TIMER_BATCH = if ($variant -in @('timer','publish')) { '1' } else { '0' }
                $env:AREAUPDATE_RADIUS_PUBLISH_BATCH = if ($variant -eq 'publish') { '1' } else { '0' }
                $mode = if ($variant -eq 'area') { 'area' } else { 'legacy' }
                $layout = if ($Stage -eq 'boundary') { 'boundary' } else { 'uniform' }
                $name = "$actors-$seed-$variant"
                & "$repositoryRoot/tools/benchmark/Measure-RuntimeCpu.ps1" -Seconds $Seconds -Actors $actors -MovesPerSecond ($actors / 35) `
                    -MapSide $side -Layout $layout -Seed $seed -JoinDurationMs 1000 -JoinIntervalMs 1 -Mode $mode -WaitMode signal `
                    -AreaAdmissionMode unlimited -AllowStarvation -ExecutablePath "$OutputDirectory/$Stage/bin/AreaUpdateExample.exe" `
                    -OutputDirectory "$OutputDirectory/$Stage/$name" | Out-Null
                $row = Get-Content "$root/$name/summary.json" -Raw | ConvertFrom-Json
                if ($row.taskStatisticsEnabled -ne $false -or $row.neverUpdated -ne 0) { throw 'Invalid performance run' }
                $row | Add-Member variant $variant
                $row | Add-Member radiusTimerBatch $env:AREAUPDATE_RADIUS_TIMER_BATCH
                $row | Add-Member radiusPublishBatch $env:AREAUPDATE_RADIUS_PUBLISH_BATCH
                $rows += $row
                $rows | Export-Csv "$root/results.csv" -NoTypeInformation
                Get-Content "$root/$name/runtime.log" | Where-Object { $_ -match '^RADIUS_(TIMERS|PUBLICATION|PROFILE)' }
                '{0}: FPS={1:F3} CPU={2:F2}% p99={3} maxGap={4} minFPS={5}' -f $name,$row.steadyActorFps,$row.steadyCpuPercent,$row.p99GapBucketMs,$row.maxGapMs,$row.steadyMinActorFps
            }
        }
    }
} finally {
    foreach ($key in $saved.Keys) { [Environment]::SetEnvironmentVariable($key, $saved[$key], 'Process') }
}
