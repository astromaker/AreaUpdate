
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$ErrorActionPreference = 'Stop'
$relativeRoot = 'artifacts/fixed-period'
$root = Join-Path $repositoryRoot $relativeRoot
$before = Join-Path $root 'before/AreaUpdateExample.exe'
if (-not (Test-Path $before)) { throw 'Preserved relative-scheduling executable is required' }
$after = Join-Path $root 'after/AreaUpdateExample.exe'
if (Test-Path $after) { throw 'Fixed-period campaign already exists; preserve its executable and results' }
New-Item -ItemType Directory -Path (Split-Path $after) -Force | Out-Null
Copy-Item (Join-Path $repositoryRoot 'Source/x64/Release/AreaUpdateExample.exe') $after
$results = @()
foreach ($round in 1..3) {
    $variants = if ($round % 2 -eq 1) { @('before','after') } else { @('after','before') }
    foreach ($variant in $variants) {
        $relative = "$relativeRoot/$variant/run-$round"
        & (Join-Path $repositoryRoot 'tools/benchmark/Measure-RuntimeCpu.ps1') -Seconds 20 -Actors 10000 -MovesPerSecond 286 -ActorWork 0 -MapSide 121 -Mode job -ExecutablePath "$relativeRoot/$variant/AreaUpdateExample.exe" -OutputDirectory $relative
        $result = Get-Content (Join-Path $repositoryRoot "$relative/summary.json") -Raw | ConvertFrom-Json
        $expectedPolicy = if ($variant -eq 'before') { 'relative' } else { 'fixed' }
        if ($result.objectSchedule -ne $expectedPolicy) { throw 'Scheduling policy mismatch' }
        $result | Add-Member -NotePropertyName variant -NotePropertyValue $variant
        $result | Add-Member -NotePropertyName round -NotePropertyValue $round
        $results += $result
        $results | Export-Csv (Join-Path $root 'results.csv') -NoTypeInformation
    }
}
$results | Select-Object variant,round,actorFps,steadyCpuPercent,maxTimerDelayMs,p99GapBucketMs,objectSkippedTicks | Format-Table -AutoSize
