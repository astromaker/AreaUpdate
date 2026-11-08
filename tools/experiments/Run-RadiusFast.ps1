param(
    [ValidateSet('scout','isolate','profile','final','boundary')][string]$Stage = 'scout',
    [ValidateSet('fast','merged','notify')][string]$Candidate = 'merged',
    [ValidateRange(10,60)][int]$Seconds = 20,
    [string]$OutputDirectory = 'artifacts/radius-fast'
)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = Join-Path $repositoryRoot "$OutputDirectory/$Stage"
if (Test-Path $root) { throw 'Choose a new output directory' }
New-Item -ItemType Directory "$root/bin" -Force | Out-Null
Copy-Item "$repositoryRoot/Source/x64/Release/AreaUpdateExample.exe" "$root/bin/AreaUpdateExample.exe"
Copy-Item "$repositoryRoot/Source/x64/Release/AreaUpdateExample.pdb" "$root/bin/AreaUpdateExample.pdb"
$saved = @{}
$keys = @('AREAUPDATE_RADIUS_PROFILE','AREAUPDATE_RADIUS_BATCH','AREAUPDATE_RADIUS_TIMER_BATCH','AREAUPDATE_RADIUS_PUBLISH_BATCH','AREAUPDATE_RADIUS_FAST_BUCKET','AREAUPDATE_RADIUS_MERGE_NOTIFY')
foreach ($key in $keys) { $saved[$key] = [Environment]::GetEnvironmentVariable($key, 'Process') }
$rows = @()
try {
    $actorsList = if ($Stage -eq 'final') { @(70000,140000,280000) } elseif ($Stage -eq 'boundary') { @(70000) } else { @(280000) }
    $seeds = if ($Stage -eq 'profile') { @(42) } else { @(42,43) }
    foreach ($actors in $actorsList) {
        $side = switch ($actors) { 70000 {320} 140000 {453} 280000 {640} }
        foreach ($seed in $seeds) {
            $variants = if ($Stage -eq 'scout') { @('area','base','fast','merged') } elseif ($Stage -eq 'isolate') { @('notify','merged') } elseif ($Stage -eq 'profile') { @('base','fast','notify','merged') } else { @('area','base',$Candidate) }
            if ($seed -eq 43) { [array]::Reverse($variants) }
            foreach ($variant in $variants) {
                $env:AREAUPDATE_RADIUS_PROFILE = if ($Stage -eq 'profile') { '64' } else { '0' }
                $env:AREAUPDATE_RADIUS_BATCH = '64'
                $env:AREAUPDATE_RADIUS_TIMER_BATCH = '1'
                $env:AREAUPDATE_RADIUS_PUBLISH_BATCH = '1'
                $env:AREAUPDATE_RADIUS_FAST_BUCKET = if ($variant -in @('fast','merged')) { '1' } else { '0' }
                $env:AREAUPDATE_RADIUS_MERGE_NOTIFY = if ($variant -in @('merged','notify')) { '1' } else { '0' }
                $mode = if ($variant -eq 'area') { 'area' } else { 'legacy' }
                $layout = if ($Stage -eq 'boundary') { 'boundary' } else { 'uniform' }
                $name = "$actors-$seed-$variant"
                & "$repositoryRoot/tools/benchmark/Measure-RuntimeCpu.ps1" -Seconds $Seconds -Actors $actors -MovesPerSecond ($actors / 35) `
                    -MapSide $side -Layout $layout -Seed $seed -JoinDurationMs 1000 -JoinIntervalMs 1 -Mode $mode -WaitMode signal `
                    -AreaAdmissionMode unlimited -AllowStarvation -ExecutablePath "$OutputDirectory/$Stage/bin/AreaUpdateExample.exe" `
                    -OutputDirectory "$OutputDirectory/$Stage/$name" | Out-Null
                $row = Get-Content "$root/$name/summary.json" -Raw | ConvertFrom-Json
                if ($row.taskStatisticsEnabled -ne $false -or $row.neverUpdated -ne 0) { throw 'Invalid performance run' }
                $lines = @(Get-Content "$root/$name/runtime.log")
                if ($mode -eq 'legacy') {
                    $expected = '^RADIUS_TUNING fast=' + $env:AREAUPDATE_RADIUS_FAST_BUCKET + ' merge=' + $env:AREAUPDATE_RADIUS_MERGE_NOTIFY + ' '
                    if (@($lines | Where-Object { $_ -match $expected }).Count -ne 1) { throw 'Radius tuning configuration mismatch' }
                }
                $row | Add-Member variant $variant
                $row | Add-Member radiusFastBucket $env:AREAUPDATE_RADIUS_FAST_BUCKET
                $row | Add-Member radiusMergeNotify $env:AREAUPDATE_RADIUS_MERGE_NOTIFY
                $rows += $row
                $rows | Export-Csv "$root/results.csv" -NoTypeInformation
                $lines | Where-Object { $_ -match '^RADIUS_(TUNING|PROFILE)' }
                '{0}: FPS={1:F3} CPU={2:F2}% p99={3} maxGap={4} minFPS={5}' -f $name,$row.steadyActorFps,$row.steadyCpuPercent,$row.p99GapBucketMs,$row.maxGapMs,$row.steadyMinActorFps
            }
        }
    }
} finally {
    foreach ($key in $saved.Keys) { [Environment]::SetEnvironmentVariable($key, $saved[$key], 'Process') }
}
