param([switch]$Fix)

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
        '--checks=-*,readability-braces-around-statements')
    if ($Fix) { $arguments += '-fix' }
    else { $arguments += '--warnings-as-errors=readability-braces-around-statements' }
    $arguments += @('--', '-std=c++17', '-DNOMINMAX', '-DAREAUPDATE_TEST_HOOKS') + $includes
    $output = @(& $tidy.Source @arguments 2>&1)
    if ($LASTEXITCODE -ne 0) {
        $output | ForEach-Object { Write-Output $_ }
        throw "Control brace check failed: $($source.Name)"
    }
    Write-Output "$($source.Name): $(if ($Fix) { 'brace fixes applied' } else { 'brace check passed' })"
}
