$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$tidy = Get-Command clang-tidy -ErrorAction Stop
$sources = @(Get-ChildItem -LiteralPath (Join-Path $root 'Source') -Recurse -File -Filter '*.cpp' |
    Where-Object { $_.FullName -notmatch '[\\/](Baseline|x64|Win32|x86|Debug|Release)[\\/]|OriginalAreaAdapter\.' } |
    Sort-Object FullName)
$includes = @('Core', 'AreaUpdate', 'ObjectUpdate', 'AreaUpdateLegacy') |
    ForEach-Object { '-I' + (Join-Path $root "Source/$_") }

foreach ($source in $sources) {
    $arguments = @($source.FullName, "--config-file=$(Join-Path $root '.clang-tidy')",
        '--warnings-as-errors=*', '--', '-std=c++17', '-DNOMINMAX', '-DAREAUPDATE_TEST_HOOKS') + $includes
    $output = @(& $tidy.Source @arguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        $output | ForEach-Object { Write-Output $_ }
        throw "Code style check failed: $($source.Name)"
    }

    Write-Output "$($source.Name): naming, initialization and braces passed"
}
