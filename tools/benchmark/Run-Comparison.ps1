param([int]$Actors = 2000, [int]$Ticks = 30, [int]$Repeats = 3, [int]$Workers = 8)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

$ErrorActionPreference = 'Stop'
$root = $repositoryRoot
$exe = Join-Path $root 'Source/x64/Release/UpdateComparison.exe'
$output = Join-Path $root 'artifacts/comparison'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$rows = @()
foreach ($layout in @('clustered', 'hotspot')) {
    foreach ($interactions in @(0, 8, 32)) {
        foreach ($repeat in 1..$Repeats) {
            $log = Join-Path $output "$layout-$interactions-$repeat.log"
            $errLog = Join-Path $output "$layout-$interactions-$repeat.stderr.log"
            $process = Start-Process -FilePath $exe -ArgumentList @('all', $Actors, $Ticks, $interactions, 1000, $Workers, 42, $layout, 0) -WindowStyle Hidden -PassThru -RedirectStandardOutput $log -RedirectStandardError $errLog
            $timer = [Diagnostics.Stopwatch]::StartNew()
            while (-not $process.WaitForExit(1000)) {
                if ($timer.Elapsed.TotalSeconds -gt 120) { $process.Kill(); throw "Comparison timeout: $log" }
            }
            $process.WaitForExit()
            if ($process.ExitCode -ne 0) { throw "Comparison failed: $log" }
            foreach ($line in Get-Content -LiteralPath $log) {
                $values = [ordered]@{ repeat = $repeat }
                foreach ($match in [regex]::Matches($line, '(\w+)=([^ ]+)')) { $values[$match.Groups[1].Value] = $match.Groups[2].Value }
                $rows += [pscustomobject]$values
            }
            $rows | Export-Csv -LiteralPath (Join-Path $output 'summary.csv') -NoTypeInformation
            Write-Output "$layout interactions=$interactions repeat=$repeat verified"
        }
    }
}
