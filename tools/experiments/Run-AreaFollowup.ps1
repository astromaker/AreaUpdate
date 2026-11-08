param(
    [string]$OutputDirectory = 'artifacts/area-validation/followup',
    [int]$AdmissionMax = 16,
    [int]$AdmissionWaitMs = 10,
    [int]$StressSeconds = 180
)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

$ErrorActionPreference='Stop'
$root=Join-Path $repositoryRoot $OutputDirectory
if(Test-Path $root){throw 'Choose a new output directory'}
New-Item -ItemType Directory $root|Out-Null
Copy-Item "$repositoryRoot/Source/x64/Release/AreaUpdateExample.exe","$repositoryRoot/Source/x64/Release/UpdateComparison.exe" $root
$results=@()
function Invoke-Case($name,$seconds,$actors,$side,$moves,$layout,$policy,$cap,$wait,$mode='area',$seed=42,$trace=0,$profile=0){
    $output="$OutputDirectory/$name"
    & "$repositoryRoot/tools/benchmark/Measure-RuntimeCpu.ps1" -Seconds $seconds -Actors $actors -MapSide $side -MovesPerSecond $moves -Layout $layout -AreaAdmissionMode $policy -AdmissionMax $cap -AdmissionWaitMs $wait -Mode $mode -Seed $seed -TimerTraceThresholdMs $trace -AreaProfileStride $profile -ExecutablePath "$OutputDirectory/AreaUpdateExample.exe" -OutputDirectory $output|Out-Null
    $s=Get-Content (Join-Path $repositoryRoot "$output/summary.json") -Raw|ConvertFrom-Json
    $s|Add-Member -NotePropertyName caseName -NotePropertyValue $name
    $script:results+=$s
    $script:results|Export-Csv "$root/results.csv" -NoTypeInformation
    '{0}: steadyFPS={1:F3} p99bucket={2} maxgap={3} CPU={4:F2}%' -f $name,$s.steadyActorFps,$s.p99GapBucketMs,$s.maxGapMs,$s.steadyCpuPercent
}
# 2: selective tracing is diagnostic, not a production performance result.
Invoke-Case 'phase-fixed' 20 280000 640 8000 'uniform' 'fixed' 8 25 'area' 42 20 64
Invoke-Case 'phase-selected' 20 280000 640 8000 'uniform' 'adaptive' $AdmissionMax $AdmissionWaitMs 'area' 42 20 64
# 3: runtime concentration, and bursty movement.
foreach($layout in @('hotspot','clustered','uniform')){
    $moves=if($layout -eq 'hotspot'){0}elseif($layout -eq 'clustered'){20000}else{80000}
    $actors=if($layout -eq 'uniform'){140000}else{70000}
    $side=if($layout -eq 'uniform'){453}else{320}
    Invoke-Case "$layout-fixed" 20 $actors $side $moves $layout 'fixed' 8 25
    Invoke-Case "$layout-selected" 20 $actors $side $moves $layout 'adaptive' $AdmissionMax $AdmissionWaitMs
    Invoke-Case "$layout-object" 20 $actors $side $moves $layout 'adaptive' $AdmissionMax $AdmissionWaitMs 'job'
}
# The independent benchmark verifies checksums/events against single-thread execution.
foreach($layout in @('uniform','clustered','hotspot')){
    foreach($seed in @(42,43)){
        $log="$root/interaction-$layout-$seed.log"
        $process=Start-Process -FilePath "$root/UpdateComparison.exe" -ArgumentList @('all',2000,40,64,0,8,$seed,$layout,50,5,320) -WindowStyle Hidden -PassThru -RedirectStandardOutput $log -RedirectStandardError "$log.stderr"
        if(-not $process.WaitForExit(180000)){$process.Kill();$process.WaitForExit();throw 'Interaction benchmark watchdog'}
        if($process.ExitCode -ne 0){throw "Interaction verification failed: $layout/$seed"}
        "interaction $layout/$seed passed"
    }
}
foreach($layout in @('clustered','hotspot')){
    foreach($seed in @(42,43)){
        $log="$root/runtime-interaction-$layout-$seed.log"
        $process=Start-Process -FilePath "$root/UpdateComparison.exe" -ArgumentList @('area-runtime',2000,10,64,0,8,$seed,$layout,0,0,320) -WindowStyle Hidden -PassThru -RedirectStandardOutput $log -RedirectStandardError "$log.stderr"
        if(-not $process.WaitForExit(120000)){$process.Kill();$process.WaitForExit();throw 'Runtime interaction watchdog'}
        if($process.ExitCode -ne 0){throw "Runtime interaction failed: $layout/$seed"}
        "actual AreaExecuter interaction $layout/$seed passed"
    }
}
# 4: reversed order across two seeds; no tracing.
Invoke-Case 'stress-selected-42' $StressSeconds 280000 640 8000 'uniform' 'adaptive' $AdmissionMax $AdmissionWaitMs 'area' 42
Invoke-Case 'stress-old-42' $StressSeconds 280000 640 8000 'uniform' 'adaptive' 64 25 'area' 42
Invoke-Case 'stress-old-43' $StressSeconds 280000 640 8000 'uniform' 'adaptive' 64 25 'area' 43
Invoke-Case 'stress-selected-43' $StressSeconds 280000 640 8000 'uniform' 'adaptive' $AdmissionMax $AdmissionWaitMs 'area' 43
Invoke-Case 'memory-no-moves' 90 280000 640 0 'uniform' 'adaptive' $AdmissionMax $AdmissionWaitMs
foreach($layout in @('clustered','hotspot')){
    $log="$root/runtime-interaction-5000-$layout.log"
    $process=Start-Process -FilePath "$root/UpdateComparison.exe" -ArgumentList @('area-runtime',5000,10,64,0,8,42,$layout,0,0,320) -WindowStyle Hidden -PassThru -RedirectStandardOutput $log -RedirectStandardError "$log.stderr"
    if(-not $process.WaitForExit(120000)){$process.Kill();$process.WaitForExit();throw '5000 Actor interaction watchdog'}
    if($process.ExitCode -ne 0){throw '5000 Actor interaction verification failed'}
    "5000 Actor actual AreaExecuter interaction $layout passed"
}
