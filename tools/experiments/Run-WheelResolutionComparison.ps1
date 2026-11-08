param([ValidateRange(20,120)][int]$Seconds = 30)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$relativeRoot = 'artifacts/wheel-resolution'
$root = Join-Path $repositoryRoot $relativeRoot
if (Test-Path (Join-Path $root 'results.csv')) { throw 'Preserve existing wheel comparison results' }
foreach ($variant in @('5ms','1ms')) {
    if (-not (Test-Path (Join-Path $root "$variant/AreaUpdateExample.exe"))) { throw "Missing $variant executable" }
}
$results = @()
foreach ($round in 1..3) {
    $variants = if ($round % 2 -eq 1) { @('5ms','1ms') } else { @('1ms','5ms') }
    foreach ($variant in $variants) {
        $relative = "$relativeRoot/$variant/run-$round"
        & (Join-Path $repositoryRoot 'tools/benchmark/Measure-RuntimeCpu.ps1') -Seconds $Seconds -Actors 10000 -MovesPerSecond 286 -MapSide 121 -Mode job -TimerTraceThresholdMs 50 -ExecutablePath "$relativeRoot/$variant/AreaUpdateExample.exe" -OutputDirectory $relative
        $result = Get-Content (Join-Path $repositoryRoot "$relative/summary.json") -Raw | ConvertFrom-Json
        $expected = if ($variant -eq '1ms') { 1 } else { 5 }
        if ($result.timerSlots -ne 256 -or $result.timerSlotMs -ne $expected -or $result.objectSchedule -ne 'fixed') {
            throw 'Wheel configuration mismatch'
        }
        if ($result.retainedTraces -gt 256) { throw 'Trace buffer exceeded bound' }
        if ($result.maxTimerDelayMs -ge 50) {
            $traces = Import-Csv (Join-Path $repositoryRoot "$relative/timer-traces.csv")
            $maximum = ($traces | Measure-Object delay_ms -Maximum).Maximum
            if ($maximum -ne $result.maxTimerDelayMs) { throw 'Largest timer delay was not retained' }
        }
        $result | Add-Member -NotePropertyName variant -NotePropertyValue $variant
        $result | Add-Member -NotePropertyName round -NotePropertyValue $round
        $result | Add-Member -NotePropertyName tracing -NotePropertyValue $true
        $results += $result
        $results | Export-Csv (Join-Path $root 'results.csv') -NoTypeInformation
    }
}
foreach ($variant in @('5ms','1ms')) {
    $relative = "$relativeRoot/$variant/trace-off"
    & (Join-Path $repositoryRoot 'tools/benchmark/Measure-RuntimeCpu.ps1') -Seconds 20 -Actors 10000 -MovesPerSecond 286 -MapSide 121 -Mode job -TimerTraceThresholdMs 0 -ExecutablePath "$relativeRoot/$variant/AreaUpdateExample.exe" -OutputDirectory $relative
    $result = Get-Content (Join-Path $repositoryRoot "$relative/summary.json") -Raw | ConvertFrom-Json
    $result | Add-Member -NotePropertyName variant -NotePropertyValue $variant
    $result | Add-Member -NotePropertyName round -NotePropertyValue 0
    $result | Add-Member -NotePropertyName tracing -NotePropertyValue $false
    $results += $result
    $results | Export-Csv (Join-Path $root 'results.csv') -NoTypeInformation
}
$results | Select-Object variant,round,tracing,actorFps,steadyCpuPercent,timerP50Ms,timerP99Ms,timerP999Ms,maxTimerDelayMs,slowTimerTasks | Format-Table -AutoSize
