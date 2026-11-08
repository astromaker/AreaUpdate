param([string]$OutputDirectory = 'artifacts/area-admission-optimization/final')
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = Join-Path $repositoryRoot $OutputDirectory
if (Test-Path $root) { throw 'Choose a new output directory' }
New-Item -ItemType Directory -Path $root | Out-Null
$results = @()
foreach ($actors in @(140000,280000)) {
    $side = if ($actors -eq 140000) {453} else {640}
    $moves = if ($actors -eq 140000) {4000} else {8000}
    $run = 0
    foreach ($version in @('before','candidate','candidate','before')) {
        ++$run
        $output = "$OutputDirectory/$actors-$run-$version"
        & (Join-Path $repositoryRoot 'tools/benchmark/Measure-RuntimeCpu.ps1') -Seconds 20 -Actors $actors -MapSide $side -MovesPerSecond $moves -Mode area -ExecutablePath "artifacts/area-admission-optimization/$version/AreaUpdateExample.exe" -OutputDirectory $output | Out-Null
        $value = Get-Content (Join-Path $repositoryRoot "$output/summary.json") -Raw | ConvertFrom-Json
        if ($value.waitMode -ne 'signal' -or $value.areaSchedule -ne 'fixed' -or $value.taskLifetimeVerified -ne $true -or $value.neverUpdated -ne 0 -or $value.processExitCode -ne 0) { throw 'Runtime validation failed' }
        $value | Add-Member -NotePropertyName version -NotePropertyValue $version
        $results += $value
        $results | Export-Csv (Join-Path $root 'results.csv') -NoTypeInformation
        Write-Output ("actors={0} version={1} FPS={2:F3} CPU={3:F2}%" -f $actors,$version,$value.actorFps,$value.steadyCpuPercent)
    }
}
