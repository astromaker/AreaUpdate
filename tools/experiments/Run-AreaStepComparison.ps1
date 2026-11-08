param(
    [Parameter(Mandatory)][string]$Baseline,
    [Parameter(Mandatory)][string]$Candidate,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [ValidateRange(10, 3600)][int]$Seconds = 30,
    [int[]]$ActorCounts = @(140000, 280000)
)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = Join-Path $repositoryRoot $OutputDirectory
New-Item -ItemType Directory $root -Force | Out-Null
$rows = @()
foreach ($actors in $ActorCounts) {
    foreach ($seed in @(42, 43)) {
        $variants = if ($seed -eq 42) { @($Baseline, $Candidate) } else { @($Candidate, $Baseline) }
        foreach ($variant in $variants) {
            $name = "$actors-$seed-$variant"
            $output = "$OutputDirectory/$name"
            $executable = "artifacts/area-followup-ab/bin/$variant.exe"
            $hash = (Get-FileHash "$repositoryRoot/$executable").Hash
            if ((Test-Path "$repositoryRoot/$output/summary.json") -eq $false) {
                & "$repositoryRoot/tools/benchmark/Measure-RuntimeCpu.ps1" -Seconds $Seconds -Actors $actors `
                    -MovesPerSecond $(if ($actors -eq 140000) { 4000 } else { 8000 }) `
                    -MapSide $(if ($actors -eq 140000) { 453 } else { 640 }) `
                    -JoinDurationMs 1000 -JoinIntervalMs 1 -Seed $seed -Mode area -WaitMode signal -AreaAdmissionMode adaptive `
                    -ExecutablePath $executable -OutputDirectory $output | Out-Null
            }

            $value = Get-Content "$repositoryRoot/$output/summary.json" -Raw | ConvertFrom-Json
            if ($value.seed -ne $seed -or $value.actors -ne $actors -or $value.executableSha256 -ne $hash) {
                throw 'Measurement configuration or executable hash mismatch'
            }
            $value | Add-Member -NotePropertyName variant -NotePropertyValue $variant
            $rows += $value
            $rows | Export-Csv "$root/results.csv" -NoTypeInformation
            '{0}: FPS={1:F3} CPU={2:F2}% p99={3}ms tasks={4}' -f $name, $value.steadyActorFps, $value.steadyCpuPercent, $value.p99GapBucketMs, $value.tasksCreated
        }
    }
}
