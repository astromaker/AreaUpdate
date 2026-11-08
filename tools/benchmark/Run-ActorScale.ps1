param(
    [ValidateRange(15,120)][int]$Seconds = 20,
    [ValidateRange(1,1000000)][int[]]$ActorCounts = @(10000,30000,70000,140000,280000,560000,1000000)
)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$relativeRoot = 'artifacts/actor-scale'
$root = Join-Path $repositoryRoot $relativeRoot
New-Item -ItemType Directory -Path $root -Force | Out-Null
$archive = Join-Path $root 'AreaUpdateExample-measured.exe'
if (Test-Path $archive) { throw 'Actor scaling campaign already exists; preserve its binary and results' }
Copy-Item (Join-Path $repositoryRoot 'Source/x64/Release/AreaUpdateExample.exe') $archive
$results = @()
$plans = @()
$actorBytes = 1200
$areaBytes = 600
$lastActors = 0
$lastSide = 0
$lastMoves = 0
foreach ($actors in $ActorCounts | Sort-Object -Unique) {
    $side = [int][math]::Ceiling(320 * [math]::Sqrt($actors/70000.0))
    $moves = [int][math]::Ceiling($actors * 2000.0 / 70000)
    [GC]::Collect()
    $memory = [GC]::GetGCMemoryInfo()
    $headroom = [math]::Max([int64]0, $memory.TotalAvailableMemoryBytes - $memory.MemoryLoadBytes)
    $budget = $headroom * 0.7
    $estimate = [double]$actors * ($actorBytes + 128) + [double]$side * $side * ($areaBytes + 128) + 64MB
    $allowed = $estimate -le $budget -and $side -le 2048
    $plans += [pscustomobject]@{ actors=$actors; mapSide=$side; movesPerSecond=$moves; estimatedMB=$estimate/1MB; budgetMB=$budget/1MB; allowed=$allowed }
    $plans | Export-Csv (Join-Path $root 'plan.csv') -NoTypeInformation
    if (-not $allowed) {
        Write-Output "Stopped before $actors Actors: memory safety budget or map limit"
        break
    }
    $relative = "$relativeRoot/actors-$actors"
    & (Join-Path $repositoryRoot 'tools/benchmark/Measure-RuntimeCpu.ps1') -Seconds $Seconds -Actors $actors -MovesPerSecond $moves -ActorWork 0 -MapSide $side -AllowStarvation -ExecutablePath "$relativeRoot/AreaUpdateExample-measured.exe" -OutputDirectory $relative
    $result = Get-Content (Join-Path $repositoryRoot "$relative/summary.json") -Raw | ConvertFrom-Json
    if ($null -eq $result.steadyCpuPercent) { throw 'No samples after full admission; increase duration' }
    $results += $result
    $results | Export-Csv (Join-Path $root 'results.csv') -NoTypeInformation
    $actorBytes = $result.actorBytes
    $areaBytes = $result.areaBytes
    $lastActors = $actors
    $lastSide = $side
    $lastMoves = $moves
    if ($result.steadyCpuPercent -ge 90) { break }
}
if ($lastActors -gt 0) {
    $relative = "$relativeRoot/actors-$lastActors-repeat"
    & (Join-Path $repositoryRoot 'tools/benchmark/Measure-RuntimeCpu.ps1') -Seconds $Seconds -Actors $lastActors -MovesPerSecond $lastMoves -ActorWork 0 -MapSide $lastSide -AllowStarvation -ExecutablePath "$relativeRoot/AreaUpdateExample-measured.exe" -OutputDirectory $relative
    $result = Get-Content (Join-Path $repositoryRoot "$relative/summary.json") -Raw | ConvertFrom-Json
    $results += $result
    $results | Export-Csv (Join-Path $root 'results.csv') -NoTypeInformation
}
$results | Select-Object actors,mapSide,actorWork,steadyCpuPercent,actorFps,peakSamplePrivateMB,minActivePerPartition,neverUpdated | Format-Table -AutoSize
