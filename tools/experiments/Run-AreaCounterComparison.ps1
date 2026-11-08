param([string]$OutputDirectory = 'artifacts/area-counter-ab', [int]$Seconds = 30)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = Join-Path $repositoryRoot $OutputDirectory
$rows = @()
foreach ($actors in @(140000, 280000)) {
    foreach ($seed in @(42, 43)) {
        $variants = if ($seed -eq 42) { @('counter-on', 'counter-off') } else { @('counter-off', 'counter-on') }
        foreach ($variant in $variants) {
            $name = "$actors-$seed-$variant"
            $output = "$OutputDirectory/$name"
            $hash = (Get-FileHash "$root/bin/$variant.exe").Hash
            if ((Test-Path "$repositoryRoot/$output/summary.json") -eq $false) {
                & "$repositoryRoot/tools/benchmark/Measure-RuntimeCpu.ps1" -Seconds $Seconds -Actors $actors `
                    -MovesPerSecond $(if ($actors -eq 140000) { 4000 } else { 8000 }) `
                    -MapSide $(if ($actors -eq 140000) { 453 } else { 640 }) `
                    -JoinDurationMs 1000 -JoinIntervalMs 1 -Seed $seed -Mode area -WaitMode signal -AreaAdmissionMode adaptive `
                    -ExecutablePath "$OutputDirectory/bin/$variant.exe" -OutputDirectory $output | Out-Null
            }

            $value = Get-Content "$repositoryRoot/$output/summary.json" -Raw | ConvertFrom-Json
            $value | Add-Member -NotePropertyName variant -NotePropertyValue $variant
            if ($value.seed -ne $seed) { throw 'Measurement seed mismatch' }
            if ($value.executableSha256 -ne $hash) { throw 'Measurement executable hash mismatch' }
            $rows += $value
            $rows | Export-Csv "$root/results.csv" -NoTypeInformation
            '{0}: FPS={1:F3} CPU={2:F2}% p99={3}ms' -f $name, $value.steadyActorFps, $value.steadyCpuPercent, $value.p99GapBucketMs
        }
    }
}
