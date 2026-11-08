param(
    [ValidateSet('profile','timers','batch','final','boundary')][string]$Stage = 'profile',
    [ValidateRange(10,60)][int]$Seconds = 20,
    [string]$OutputDirectory = 'artifacts/radius-followup'
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
    $env:AREAUPDATE_RADIUS_PUBLISH_BATCH = '0'
    $actorsList = if ($Stage -eq 'profile') { @(70000,280000) } elseif ($Stage -eq 'timers') { @(140000,280000) } elseif ($Stage -eq 'batch') { @(280000) } elseif ($Stage -eq 'boundary') { @(70000) } else { @(70000,140000,280000) }
    $seeds = if ($Stage -eq 'profile') { @(42) } else { @(42,43) }
    foreach ($actors in $actorsList) {
        $side = switch ($actors) { 70000 {320} 140000 {453} 280000 {640} }
        foreach ($seed in $seeds) {
            $variants = switch ($Stage) {
                profile { @('legacy-single-64') }
                timers { if ($seed -eq 42) { @('legacy-single-64','legacy-grouped-64') } else { @('legacy-grouped-64','legacy-single-64') } }
                batch { if ($seed -eq 42) { @('legacy-grouped-1','legacy-grouped-4','legacy-grouped-16','legacy-grouped-64') } else { @('legacy-grouped-64','legacy-grouped-16','legacy-grouped-4','legacy-grouped-1') } }
                final { if ($seed -eq 42) { @('area-reference','legacy-before','legacy-final') } else { @('legacy-final','legacy-before','area-reference') } }
                boundary { if ($seed -eq 42) { @('area-reference','legacy-final') } else { @('legacy-final','area-reference') } }
            }
            foreach ($variant in $variants) {
                $env:AREAUPDATE_RADIUS_PROFILE = if ($Stage -eq 'profile') { '64' } else { '0' }
                if ($Stage -in @('final','boundary')) {
                    $env:AREAUPDATE_RADIUS_BATCH = $saved['AREAUPDATE_RADIUS_BATCH']
                    $env:AREAUPDATE_RADIUS_TIMER_BATCH = $saved['AREAUPDATE_RADIUS_TIMER_BATCH']
                    if ($variant -eq 'legacy-before') {
                        $env:AREAUPDATE_RADIUS_BATCH = '64'
                        $env:AREAUPDATE_RADIUS_TIMER_BATCH = '0'
                    }
                } else {
                    $env:AREAUPDATE_RADIUS_BATCH = ($variant -split '-')[-1]
                    $env:AREAUPDATE_RADIUS_TIMER_BATCH = if ($variant -match 'grouped') { '1' } else { '0' }
                }
                $mode = if ($variant -eq 'area-reference') { 'area' } else { 'legacy' }
                $layout = if ($Stage -eq 'boundary') { 'boundary' } else { 'uniform' }
                $name = "$actors-$seed-$variant"
                & "$repositoryRoot/tools/benchmark/Measure-RuntimeCpu.ps1" -Seconds $Seconds -Actors $actors -MovesPerSecond ($actors / 35) `
                    -MapSide $side -Layout $layout -Seed $seed -JoinDurationMs 1000 -JoinIntervalMs 1 -Mode $mode -WaitMode signal `
                    -AreaAdmissionMode unlimited -AllowStarvation -ExecutablePath "$OutputDirectory/$Stage/bin/AreaUpdateExample.exe" `
                    -OutputDirectory "$OutputDirectory/$Stage/$name" | Out-Null
                $row = Get-Content "$root/$name/summary.json" -Raw | ConvertFrom-Json
                if ($row.taskStatisticsEnabled -ne $false) { throw 'Performance run must disable Task statistics' }
                $lines = @(Get-Content "$root/$name/runtime.log")
                if ($mode -eq 'legacy') {
                    $timerLine = $lines | Where-Object { $_ -match '^RADIUS_TIMERS ' } | Select-Object -Last 1
                    $profileLine = $lines | Where-Object { $_ -match '^RADIUS_PROFILE ' } | Select-Object -Last 1
                    if (-not $timerLine -or -not $profileLine) { throw 'Missing radius diagnostics' }
                    $timer = @{}
                    foreach ($match in [regex]::Matches($timerLine, '(\w+)=(\d+)')) { $timer[$match.Groups[1].Value] = [uint64]$match.Groups[2].Value }
                    if ($timer.tasks -gt $timer.reservations -or ($timer.grouped -eq 0 -and $timer.tasks -ne $timer.reservations)) { throw 'Invalid timer accounting' }
                    $row | Add-Member timerReservations $timer.reservations
                    $row | Add-Member timerTasks $timer.tasks
                } else {
                    $row | Add-Member timerReservations 0
                    $row | Add-Member timerTasks 0
                }
                $row | Add-Member variant $variant
                $row | Add-Member radiusBatch $env:AREAUPDATE_RADIUS_BATCH
                $row | Add-Member radiusTimerBatch $env:AREAUPDATE_RADIUS_TIMER_BATCH
                $rows += $row
                $rows | Export-Csv "$root/results.csv" -NoTypeInformation
                $lines | Where-Object { $_ -match '^RADIUS_(PROFILE|TIMERS)' }
                '{0}: FPS={1:F3} CPU={2:F2}% gap99={3} maxGap={4} minFPS={5}' -f $name,$row.steadyActorFps,$row.steadyCpuPercent,$row.p99GapBucketMs,$row.maxGapMs,$row.steadyMinActorFps
            }
        }
    }
} finally {
    foreach ($key in $saved.Keys) { [Environment]::SetEnvironmentVariable($key, $saved[$key], 'Process') }
}
