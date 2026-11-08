param([string]$OutputDirectory = 'artifacts/area-unlimited', [ValidateRange(10, 3600)][int]$Seconds = 30)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = Join-Path $repositoryRoot $OutputDirectory
if (Test-Path "$root/results.csv") { throw 'Choose a new output directory' }
New-Item -ItemType Directory "$root/bin" -Force | Out-Null
Copy-Item "$repositoryRoot/Source/x64/Release/AreaUpdateExample.exe" "$root/bin/AreaUpdateExample.exe"
Copy-Item "$repositoryRoot/Source/x64/Release/AreaUpdateExample.pdb" "$root/bin/AreaUpdateExample.pdb"
$rows = @()
$cases = @(
    @{ name = 'uniform-140k'; actors = 140000; side = 453; moves = 4000; layout = 'uniform' },
    @{ name = 'uniform-280k'; actors = 280000; side = 640; moves = 8000; layout = 'uniform' },
    @{ name = 'skew-280k'; actors = 280000; side = 640; moves = 0; layout = 'partition-skew' }
)
foreach ($case in $cases) {
    foreach ($seed in @(42, 43)) {
        $policies = if ($seed -eq 42) { @('adaptive', 'unlimited') } else { @('unlimited', 'adaptive') }
        foreach ($policy in $policies) {
            $output = "$OutputDirectory/$($case.name)-$seed-$policy"
            & "$repositoryRoot/tools/benchmark/Measure-RuntimeCpu.ps1" -Seconds $Seconds -Actors $case.actors `
                -MovesPerSecond $case.moves -MapSide $case.side -Layout $case.layout -Seed $seed `
                -JoinDurationMs 1000 -JoinIntervalMs 1 -Mode area -WaitMode signal `
                -AreaAdmissionMode $policy -AllowStarvation -ExecutablePath "$OutputDirectory/bin/AreaUpdateExample.exe" `
                -OutputDirectory $output | Out-Null

            $value = Get-Content "$repositoryRoot/$output/summary.json" -Raw | ConvertFrom-Json
            if ($value.steadyFairnessAvailable -eq $false -or $value.areaAdmissionMode -ne $policy) {
                throw 'Fairness metrics or admission mode missing'
            }
            $parts = Get-Content "$repositoryRoot/$output/runtime.log" | Select-String '^PARTITION '
            $counts = @($parts | ForEach-Object { [double]([regex]::Match($_.Line, 'areas_popped=(\d+)').Groups[1].Value) })
            $value | Add-Member -NotePropertyName caseName -NotePropertyValue $case.name
            $value | Add-Member -NotePropertyName minPartitionUpdates -NotePropertyValue (($counts | Measure-Object -Minimum).Minimum)
            $value | Add-Member -NotePropertyName maxPartitionUpdates -NotePropertyValue (($counts | Measure-Object -Maximum).Maximum)
            $rows += $value
            $rows | Export-Csv "$root/results.csv" -NoTypeInformation
            '{0} seed={1} {2}: FPS={3:F3} CPU={4:F2}% minFPS={5:F2} p99={6}ms maxGap={7}ms never={8}' -f $case.name, $seed, $policy, $value.steadyActorFps, $value.steadyCpuPercent, $value.steadyMinActorFps, $value.p99GapBucketMs, $value.maxGapMs, $value.neverUpdated
        }
    }
}
