param([int]$StressSeconds = 600, [int]$SampleSeconds = 20, [string]$OutputDirectory = 'artifacts/measurements')
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

$ErrorActionPreference = 'Stop'
$root = $repositoryRoot
$output = Join-Path $root $OutputDirectory
New-Item -ItemType Directory -Path $output -Force | Out-Null
$exe = Join-Path $root 'Source/x64/Release/AreaUpdateExample.exe'
$results = @()
$cases = @()
foreach ($repeat in 1..3) {
    $cases += @{ Name = "sparse-$repeat"; Seconds = $SampleSeconds; Actors = 1000; Moves = 200 }
    $cases += @{ Name = "dense-$repeat"; Seconds = $SampleSeconds; Actors = 10000; Moves = 2000 }
}
$cases += @{ Name = 'stress'; Seconds = $StressSeconds; Actors = 5000; Moves = 20000 }
foreach ($case in $cases) {
    $log = Join-Path $output ($case.Name + '.log')
    $errLog = Join-Path $output ($case.Name + '.stderr.log')
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $process = Start-Process -FilePath $exe -ArgumentList @($case.Seconds, $case.Actors, $case.Moves, 42) -WindowStyle Hidden -PassThru -RedirectStandardOutput $log -RedirectStandardError $errLog
    $samples = @()
    $nextProgress = 30
    while (-not $process.WaitForExit(1000)) {
        $process.Refresh()
        if ($process.HasExited) { break }
        $samples += [pscustomobject]@{ Seconds = $timer.Elapsed.TotalSeconds; CpuSeconds = $process.TotalProcessorTime.TotalSeconds; PrivateMB = $process.PrivateMemorySize64 / 1MB; WorkingSetMB = $process.WorkingSet64 / 1MB }
        if ($timer.Elapsed.TotalSeconds -ge $nextProgress) {
            Write-Output ("{0}: {1:N0}s, private {2:N1} MB" -f $case.Name, $timer.Elapsed.TotalSeconds, ($process.PrivateMemorySize64 / 1MB))
            $nextProgress += 30
        }
        if ($timer.Elapsed.TotalSeconds -gt $case.Seconds + 60) {
            $process.Kill()
            throw "$($case.Name) exceeded watchdog deadline"
        }
    }
    $process.WaitForExit()
    $process.Refresh()
    $samples | Export-Csv -LiteralPath (Join-Path $output ($case.Name + '.resources.csv')) -NoTypeInformation
    if ($process.ExitCode -ne 0) { throw "$($case.Name) failed with exit $($process.ExitCode); see $log" }
    $summary = Get-Content -LiteralPath $log | Where-Object { $_ -like 'SUMMARY*' } | Select-Object -Last 1
    if (-not $summary) { throw "Missing summary for $($case.Name)" }
    $values = @{}
    foreach ($match in [regex]::Matches($summary, '(\w+)=([^ ]+)')) { $values[$match.Groups[1].Value] = $match.Groups[2].Value }
    $first = $samples | Select-Object -First 1
    $last = $samples | Select-Object -Last 1
    $results += [pscustomobject]@{
        Case = $case.Name; Actors = $case.Actors; MovesPerSecondAttempted = $case.Moves
        ElapsedSeconds = $timer.Elapsed.TotalSeconds
        CpuOneCorePercent = 100 * $process.TotalProcessorTime.TotalSeconds / $timer.Elapsed.TotalSeconds
        PrivateStartMB = $first.PrivateMB; PrivateEndMB = $last.PrivateMB
        PrivatePeakMB = ($samples | Measure-Object PrivateMB -Maximum).Maximum
        ActorFPS = $values.actor_fps; Updates = $values.updates; SuccessfulMoves = $values.moves
        Overlaps = $values.overlaps; NeverUpdated = $values.never_updated
        MaxGapMs = $values.max_gap_ms; P99GapBucketMs = $values.p99_gap_bucket_ms
        ExitCode = $process.ExitCode
    }
    $results | Export-Csv -LiteralPath (Join-Path $output 'summary.csv') -NoTypeInformation
    Write-Output "$($case.Name): $summary"
}
