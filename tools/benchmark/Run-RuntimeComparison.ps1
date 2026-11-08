param(
    [ValidateRange(15,120)][int]$Seconds = 20,
    [ValidateRange(1,1000000)][int[]]$ActorCounts = @(10000,30000,70000,140000,280000,560000)
)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$relativeRoot = 'artifacts/runtime-three-way'
$root = Join-Path $repositoryRoot $relativeRoot
New-Item -ItemType Directory -Path $root -Force | Out-Null
$archive = Join-Path $root 'AreaUpdateExample-measured.exe'
if (Test-Path $archive) { throw 'Runtime comparison already exists; preserve its binary and results' }
Copy-Item (Join-Path $repositoryRoot 'Source/x64/Release/AreaUpdateExample.exe') $archive
$results = @()
$plans = @()
$lastActors = 0
$lastSide = 0
$lastMoves = 0
$index = 0
foreach ($actors in $ActorCounts | Sort-Object -Unique) {
    $side = [int][math]::Ceiling(320 * [math]::Sqrt($actors/70000.0))
    $moves = [int][math]::Ceiling($actors * 2000.0 / 70000)
    [GC]::Collect()
    $memory = [GC]::GetGCMemoryInfo()
    $headroom = [math]::Max([int64]0, $memory.TotalAvailableMemoryBytes - $memory.MemoryLoadBytes)
    $budget = $headroom * 0.7
    $base = [double]$actors * (1016 + 128) + [double]$side * $side * (360 + 128) + 64MB
    $legacyEstimate = $base + [double]$side * $side * 352
    $jobEstimate = $base + [double]$actors * 200 + [double]$side * $side * 8
    $estimate = [math]::Max($legacyEstimate, $jobEstimate)
    $allowed = $estimate -le $budget -and $side -le 2048
    $plans += [pscustomobject]@{actors=$actors;mapSide=$side;movesPerSecond=$moves;estimatedMB=$estimate/1MB;budgetMB=$budget/1MB;allowed=$allowed}
    $plans | Export-Csv (Join-Path $root 'plan.csv') -NoTypeInformation
    if (-not $allowed) {
        Write-Output "Stopped before $actors Actors: common memory safety budget"
        break
    }
    $modes = switch ($index % 3) { 0 { @('area','original','job') } 1 { @('original','job','area') } 2 { @('job','area','original') } }
    foreach ($mode in $modes) {
        $relative = "$relativeRoot/$mode-$actors"
        & (Join-Path $repositoryRoot 'tools/benchmark/Measure-RuntimeCpu.ps1') -Seconds $Seconds -Actors $actors -MovesPerSecond $moves -ActorWork 0 -MapSide $side -Mode $mode -AllowStarvation -ExecutablePath "$relativeRoot/AreaUpdateExample-measured.exe" -OutputDirectory $relative
        $result = Get-Content (Join-Path $repositoryRoot "$relative/summary.json") -Raw | ConvertFrom-Json
        if ($null -eq $result.steadyCpuPercent) { throw 'No CPU samples after admission; increase duration' }
        $results += $result
        $results | Export-Csv (Join-Path $root 'results.csv') -NoTypeInformation
    }
    $lastActors=$actors
    $lastSide=$side
    $lastMoves=$moves
    ++$index
}
if ($lastActors -gt 0) {
    foreach ($mode in @('job','original','area')) {
        $relative = "$relativeRoot/$mode-$lastActors-repeat"
        & (Join-Path $repositoryRoot 'tools/benchmark/Measure-RuntimeCpu.ps1') -Seconds $Seconds -Actors $lastActors -MovesPerSecond $lastMoves -ActorWork 0 -MapSide $lastSide -Mode $mode -AllowStarvation -ExecutablePath "$relativeRoot/AreaUpdateExample-measured.exe" -OutputDirectory $relative
        $results += Get-Content (Join-Path $repositoryRoot "$relative/summary.json") -Raw | ConvertFrom-Json
        $results | Export-Csv (Join-Path $root 'results.csv') -NoTypeInformation
    }
}
$results | Select-Object actors,mode,steadyCpuPercent,actorFps,peakSamplePrivateMB,neverUpdated,successfulMoves | Format-Table -AutoSize
