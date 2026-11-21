param(
    [switch]$ProbeOnly,
    [switch]$DriverOnly,
    [string]$WdkRoot = (Join-Path $PSScriptRoot 'build\wdk\kit\c'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot 'dist')
)

$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Build Tools were not found.' }
$visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $visualStudio) { throw 'Install MSVC Build Tools and the Windows SDK.' }
$vcvars = Join-Path $visualStudio 'VC\Auxiliary\Build\vcvars64.bat'
$environment = & cmd.exe /d /c "call `"$vcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Unable to initialize MSVC.' }
foreach ($entry in $environment) {
    if ($entry -match '^([^=]+)=(.*)$') {
        [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
    }
}
$output = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDirectory)
$objects = Join-Path $projectRoot 'build'
if ($DriverOnly) {
    & (Join-Path $projectRoot 'driver\build-driver.ps1') -WdkRoot $WdkRoot
    return
}
$profiles = Join-Path $output 'devices'
New-Item -ItemType Directory -Path $output, $objects, $profiles -Force | Out-Null
Get-ChildItem (Join-Path $projectRoot 'devices') -Filter '*.json' | Copy-Item -Destination $profiles
$kit = Join-Path $env:WindowsSdkDir "Include\$($env:WindowsSDKVersion.TrimEnd('\'))\cppwinrt"
$compiler = @('/nologo', '/std:c++20', '/utf-8', '/O2', '/MT', '/EHsc', '/W4', '/DUNICODE', '/D_UNICODE', "/I$kit")
$systemLibraries = @('windowsapp.lib', 'ole32.lib', 'setupapi.lib', 'hid.lib', 'shell32.lib', 'propsys.lib', 'advapi32.lib', 'cfgmgr32.lib', 'winhttp.lib')

Push-Location $objects
try {
    & cl.exe @compiler '/c' (Join-Path $projectRoot 'native\flydigi_station.cpp') (Join-Path $projectRoot 'native\devices.cpp') (Join-Path $projectRoot 'native\notifications.cpp') (Join-Path $projectRoot 'native\driver_install.cpp') (Join-Path $projectRoot 'native\github_download.cpp')
    if ($LASTEXITCODE -ne 0) { throw 'Device module compilation failed.' }
    if (Test-Path -LiteralPath (Join-Path $projectRoot 'native\probe.cpp')) {
        & cl.exe @compiler (Join-Path $projectRoot 'native\probe.cpp') (Join-Path $projectRoot 'native\gameinput_probe.cpp') (Join-Path $projectRoot 'native\profile_catalog.cpp') 'flydigi_station.obj' 'devices.obj' 'notifications.obj' 'driver_install.obj' 'github_download.obj' @systemLibraries "/Fe$output\BatteryProbe.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Probe compilation failed.' }
    } elseif ($ProbeOnly) { throw 'The diagnostic sources are only available in the development workspace.' }
    if (-not $ProbeOnly) {
        & cl.exe @compiler (Join-Path $projectRoot 'native\flydigi_sample.cpp') "/Fe$output\FlydigiSample.exe"
        if ($LASTEXITCODE -ne 0) { throw 'Flydigi reader compilation failed.' }
        $resourceScript = Join-Path $objects 'flydigi-reader.rc'
        $reader = (Join-Path $output 'FlydigiSample.exe').Replace('\','/')
        Set-Content -LiteralPath $resourceScript -Value ('301 RCDATA "' + $reader + '"') -Encoding unicode
        & rc.exe /nologo /fo flydigi-reader.res $resourceScript
        if ($LASTEXITCODE -ne 0) { throw 'Embedding the Flydigi reader failed.' }
        & cl.exe @compiler 'flydigi-reader.res' (Join-Path $projectRoot 'native\app.cpp') (Join-Path $projectRoot 'native\profile_catalog.cpp') 'flydigi_station.obj' 'devices.obj' 'notifications.obj' 'driver_install.obj' 'github_download.obj' @systemLibraries 'd2d1.lib' 'dwrite.lib' 'dwmapi.lib' 'user32.lib' 'gdi32.lib' "/Fe$output\BatteryMonitor.exe" /link /SUBSYSTEM:WINDOWS
        if ($LASTEXITCODE -ne 0) { throw 'Application compilation failed.' }
        & cl.exe @compiler (Join-Path $projectRoot 'native\setup.cpp') 'shell32.lib' 'ole32.lib' 'user32.lib' "/Fe$output\BatterySetup.exe" /link /SUBSYSTEM:WINDOWS
        if ($LASTEXITCODE -ne 0) { throw 'Setup compilation failed.' }
    }
} finally { Pop-Location }
