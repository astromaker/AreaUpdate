
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$ErrorActionPreference = 'Stop'
$tokens = $null
$errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $repositoryRoot 'tools/benchmark/Run-ScenarioComparison.ps1'), [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Scenario script syntax error' }
$functions = @($ast.FindAll({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
    $node.Name -in @('Median', 'MemoryMedian')
}, $true))
if ($functions.Count -ne 2) { throw 'Aggregation helpers not found' }
foreach ($function in $functions) { . ([scriptblock]::Create($function.Extent.Text)) }

if ($null -ne (MemoryMedian @($null, '', ' '))) { throw 'Missing samples must stay missing' }
if ((MemoryMedian @($null, '', 0)) -ne 0) { throw 'Measured zero must be retained' }
$samples = @(
    [pscustomobject]@{ PrivatePeakMB = $null },
    [pscustomobject]@{ PrivatePeakMB = 100.5 },
    [pscustomobject]@{ PrivatePeakMB = 300.5 }
)
$restored = @($samples | ConvertTo-Csv -NoTypeInformation | ConvertFrom-Csv)
if ((MemoryMedian $samples.PrivatePeakMB) -ne 200.5) { throw 'In-memory median mismatch' }
if ((MemoryMedian $restored.PrivatePeakMB) -ne 200.5) { throw 'CSV median mismatch' }
Write-Output 'Scenario aggregation tests passed'
