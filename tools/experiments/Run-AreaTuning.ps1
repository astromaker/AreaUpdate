param([string]$OutputDirectory = 'artifacts/area-validation/tuning')
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

$ErrorActionPreference = 'Stop'
$root=Join-Path $repositoryRoot $OutputDirectory
if(Test-Path $root){throw 'Choose a new output directory'}
New-Item -ItemType Directory $root | Out-Null
Copy-Item "$repositoryRoot/Source/x64/Release/AreaUpdateExample.exe" "$root/AreaUpdateExample.exe"
$cases=@(
    @{name='fixed';policy='fixed';cap=8;wait=25},
    @{name='old64';policy='adaptive';cap=64;wait=25},
    @{name='cap32';policy='adaptive';cap=32;wait=10},
    @{name='cap16';policy='adaptive';cap=16;wait=10}
)
$results=@()
foreach($round in 1..2){
    $order=if($round -eq 1){$cases}else{@($cases[3],$cases[2],$cases[1],$cases[0])}
    foreach($case in $order){
        $output="$OutputDirectory/$($case.name)-$round"
        & "$repositoryRoot/tools/benchmark/Measure-RuntimeCpu.ps1" -Seconds 20 -Actors 280000 -MovesPerSecond 8000 -MapSide 640 -AreaAdmissionMode $case.policy -AdmissionMax $case.cap -AdmissionWaitMs $case.wait -ExecutablePath "$OutputDirectory/AreaUpdateExample.exe" -OutputDirectory $output | Out-Null
        $s=Get-Content (Join-Path $repositoryRoot "$output/summary.json") -Raw|ConvertFrom-Json
        $s|Add-Member -NotePropertyName caseName -NotePropertyValue $case.name
        $results+=$s
        $results|Export-Csv "$root/results.csv" -NoTypeInformation
        '{0} round={1} steadyFPS={2:F3} p99bucket={3} maxgap={4} CPU={5:F2}%' -f $case.name,$round,$s.steadyActorFps,$s.p99GapBucketMs,$s.maxGapMs,$s.steadyCpuPercent
    }
}
