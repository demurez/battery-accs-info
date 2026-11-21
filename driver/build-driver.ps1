param([Parameter(Mandatory)][string]$WdkRoot)

$ErrorActionPreference = 'Stop'
$WdkRoot = (Resolve-Path -LiteralPath $WdkRoot).Path
$version = $env:WindowsSDKVersion.TrimEnd('\')
$headers = Join-Path $WdkRoot "Include\$version"
$libraries = Join-Path $WdkRoot "Lib\$version\km\x64"
$wdfHeaders = Join-Path $WdkRoot 'Include\wdf\kmdf\1.33'
$wdfLibraries = Join-Path $WdkRoot 'Lib\wdf\kmdf\x64\1.33'
$sdk = Join-Path $env:WindowsSdkDir "Include\$version"
if (-not (Test-Path -LiteralPath (Join-Path $headers 'km\ntddk.h'))) {
    throw "Missing WDK kernel headers for SDK $version."
}

$output = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\direwolf-driver'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$source = Join-Path $PSScriptRoot 'direwolf_filter.c'
$object = Join-Path (Split-Path -Parent $output) 'direwolf_filter.obj'
$binary = Join-Path $output 'PeripheralChargeDirewolf.sys'
$compiler = @(
    '/nologo', '/c', '/TC', '/std:c17', '/kernel', '/O2', '/GS', '/guard:cf', '/W4', '/WX', '/Zl', '/utf-8',
    '/D_AMD64_', '/DAMD64', '/D_WIN64', '/D_WIN32_WINNT=0x0A00', '/DNTDDI_VERSION=0x0A00000B',
    '/DKMDF_VERSION_MAJOR=1', '/DKMDF_VERSION_MINOR=33',
    "/I$headers\km", "/I$headers\shared", "/I$sdk\shared", "/external:I$wdfHeaders", '/external:W0', "/Fo$object"
)
& cl.exe @compiler $source
if ($LASTEXITCODE -ne 0) { throw 'Kernel compilation failed.' }

$linker = @(
    '/nologo', '/driver', '/kernel', '/subsystem:native,10.0', '/machine:x64', '/entry:FxDriverEntry',
    '/dynamicbase', '/nxcompat', '/incremental:no', '/nodefaultlib', '/guard:cf',
    "/out:$binary", "/libpath:$libraries", "/libpath:$wdfLibraries",
    'ntoskrnl.lib', 'usbd.lib', 'BufferOverflowFastFailK.lib', 'wdfdriverentry.lib', 'wdfldr.lib'
)
& link.exe @linker $object
if ($LASTEXITCODE -ne 0) { throw 'Kernel link failed.' }

$inf = Join-Path $output 'PeripheralChargeDirewolf.inf'
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'PeripheralChargeDirewolf.inf') -Destination $inf
$infVerif = Join-Path $WdkRoot "tools\$version\x64\infverif.exe"
& $infVerif /w $inf
if ($LASTEXITCODE -ne 0) { throw 'INF validation failed.' }
$inf2cat = Join-Path $WdkRoot "bin\$version\x86\Inf2Cat.exe"
& $inf2cat "/driver:$output" /os:10_CO_X64,10_NI_X64,10_GE_X64 /verbose
if ($LASTEXITCODE -ne 0) { throw 'Driver catalog generation failed.' }

$submission = Join-Path (Split-Path -Parent $output) 'direwolf-submission'
New-Item -ItemType Directory -Force -Path $submission | Out-Null
$ddf = Join-Path $submission 'driver.ddf'
@"
.OPTION EXPLICIT
.Set CabinetFileCountThreshold=0
.Set FolderFileCountThreshold=0
.Set FolderSizeThreshold=0
.Set MaxCabinetSize=0
.Set MaxDiskSize=0
.Set CabinetNameTemplate=PeripheralChargeDirewolf.cab
.Set CompressionType=MSZIP
.Set Cabinet=on
.Set Compress=on
.Set DiskDirectoryTemplate=.
.Set DestinationDir=PeripheralChargeDirewolf
"$inf"
"$binary"
"@ | Set-Content -LiteralPath $ddf -Encoding oem
Push-Location $submission
try {
    & "$env:SystemRoot\System32\makecab.exe" /F $ddf
    if ($LASTEXITCODE -ne 0) { throw 'Driver submission CAB generation failed.' }
} finally { Pop-Location }

Write-Output "Unsigned driver package: $output"
Write-Output "Driver submission CAB: $(Join-Path $submission 'PeripheralChargeDirewolf.cab')"
