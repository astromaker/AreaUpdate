param([string]$OutputDirectory = 'artifacts/legacy-optimization/comparison', [int]$Seconds = 30)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = Join-Path $repositoryRoot $OutputDirectory
if (Test-Path $root) { throw 'Choose a new output directory' }
New-Item -ItemType Directory "$root/bin" | Out-Null
Copy-Item "$repositoryRoot/Source/x64/Release/AreaUpdateExample.exe" "$root/bin/AreaUpdateExample.exe"
Copy-Item "$repositoryRoot/Source/x64/Release/AreaUpdateExample.pdb" "$root/bin/AreaUpdateExample.pdb"
$rows = @()
$cases = @(
    @{ name = 'uniform-70k'; actors = 70000; side = 320; moves = 2000; layout = 'uniform' },
    @{ name = 'uniform-140k'; actors = 140000; side = 453; moves = 4000; layout = 'uniform' },
    @{ name = 'uniform-280k'; actors = 280000; side = 640; moves = 8000; layout = 'uniform' },
    @{ name = 'hotspot-70k'; actors = 70000; side = 320; moves = 2000; layout = 'hotspot' }
)
foreach ($case in $cases) {
    foreach ($seed in @(42, 43)) {
        $modes = if ($seed -eq 42) { @('area', 'legacy') } else { @('legacy', 'area') }
        foreach ($mode in $modes) {
            $output = "$OutputDirectory/$($case.name)-$seed-$mode"
            & "$repositoryRoot/tools/benchmark/Measure-RuntimeCpu.ps1" -Seconds $Seconds -Actors $case.actors `
                -MovesPerSecond $case.moves -MapSide $case.side -Layout $case.layout -Seed $seed `
                -JoinDurationMs 1000 -JoinIntervalMs 1 -Mode $mode -WaitMode signal `
                -AreaAdmissionMode unlimited -AllowStarvation -ExecutablePath "$OutputDirectory/bin/AreaUpdateExample.exe" `
                -OutputDirectory $output | Out-Null

            $value = Get-Content "$repositoryRoot/$output/summary.json" -Raw | ConvertFrom-Json
            if ($value.mode -ne $mode -or $value.taskStatisticsEnabled -ne $false) { throw 'Comparison configuration mismatch' }
            $value | Add-Member -NotePropertyName caseName -NotePropertyValue $case.name
            $rows += $value
            $rows | Export-Csv "$root/results.csv" -NoTypeInformation
            '{0} seed={1} {2}: FPS={3:F3} CPU={4:F2}% p99={5}ms maxGap={6}ms lock_rejects={7}' -f $case.name,$seed,$mode,$value.steadyActorFps,$value.steadyCpuPercent,$value.p99GapBucketMs,$value.maxGapMs,$value.radiusAdmissionFailures
        }
    }
}
