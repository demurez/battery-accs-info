param(
    [Parameter(Mandatory)][string]$SignedPackage,
    [string]$OutputDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\publish-driver')
)

$ErrorActionPreference = 'Stop'
$source = (Resolve-Path -LiteralPath $SignedPackage).Path
$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
$catalog = Join-Path $source 'PeripheralChargeDirewolf.cat'
$files = @('PeripheralChargeDirewolf.inf', 'PeripheralChargeDirewolf.sys')
$sdkBin = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
$signTool = Get-ChildItem -LiteralPath $sdkBin -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
    Sort-Object { [version]$_.Name } -Descending |
    ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' } |
    Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $signTool) { throw 'SignTool from the Windows SDK is required.' }
& $signTool verify /kp $catalog
if ($LASTEXITCODE -ne 0) { throw 'The driver catalog does not satisfy the kernel signing policy.' }
foreach ($file in $files) {
    & $signTool verify /kp /c $catalog (Join-Path $source $file)
    if ($LASTEXITCODE -ne 0) { throw "$file is not covered by the trusted driver catalog." }
}

$package = Join-Path $output 'driver-package'
New-Item -ItemType Directory -Force -Path $package | Out-Null
foreach ($file in ($files + 'PeripheralChargeDirewolf.cat')) {
    Copy-Item -LiteralPath (Join-Path $source $file) -Destination $package
}
Set-Content -LiteralPath (Join-Path $package 'version.txt') -Value 'direwolf-package-v1' -Encoding Ascii
Write-Output "Publish $package as driver-package in the repository's main branch."
