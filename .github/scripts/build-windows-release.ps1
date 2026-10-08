param(
    [Parameter(Mandatory = $true)][string]$Version
)
$ErrorActionPreference = 'Stop'

if ($Version -notmatch '^(v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?|ci-[0-9a-f]{12})$') {
    throw 'Invalid release version.'
}
$env:BONDRIVER_RELEASE_VERSION = $Version
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$testBuild = Join-Path $repo '.ci-build/windows-test'
$releaseBuild = Join-Path $repo '.ci-build/windows-release'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) { throw 'vswhere.exe was not found.' }
$vsPath = & $vswhere -latest -products '*' -version '[17.0,18.0)' `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $vsPath) { throw 'Visual Studio 2022 C++ toolset was not found.' }
$devCmd = Join-Path $vsPath 'Common7/Tools/VsDevCmd.bat'
$devCommand = "call `"$devCmd`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
$environmentLines = & $env:ComSpec /d /s /c $devCommand
if ($LASTEXITCODE -ne 0) { throw 'VsDevCmd.bat initialization failed.' }
foreach ($line in $environmentLines) {
    $separator = $line.IndexOf('=')
    if ($separator -gt 0) {
        [Environment]::SetEnvironmentVariable($line.Substring(0, $separator), $line.Substring($separator + 1), 'Process')
    }
}

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
function Assert-CtestCount([string]$BuildDir, [int]$Expected, [string]$Exclude = '') {
    $arguments = @('--test-dir', $BuildDir, '-C', 'Release', '-N')
    if ($Exclude) { $arguments += @('-E', $Exclude) }
    $output = & ctest @arguments | Out-String
    if ($LASTEXITCODE -ne 0) { throw "ctest -N failed with exit code $LASTEXITCODE" }
    $match = [regex]::Match($output, 'Total Tests:\s+(\d+)')
    if (-not $match.Success -or [int]$match.Groups[1].Value -ne $Expected) {
        throw "Expected $Expected registered tests; output was: $output"
    }
}

Invoke-Checked 'cmake' @('-S', $repo, '-B', $testBuild, '-DBUILD_TESTING=ON',
    '-DBONDRIVER_WERROR=ON', '-DBONDRIVER_STATIC_CRT=ON')
Invoke-Checked 'cmake' @('--build', $testBuild, '--config', 'Release', '--parallel', '2')
Assert-CtestCount $testBuild 39
Invoke-Checked 'ctest' @('--test-dir', $testBuild, '-C', 'Release', '--output-on-failure')

Invoke-Checked 'cmake' @('-S', $repo, '-B', $releaseBuild, '-DBUILD_TESTING=OFF',
    '-DBONDRIVER_STATIC_CRT=ON')
Invoke-Checked 'cmake' @('--build', $releaseBuild, '--config', 'Release', '--parallel', '2')

$releaseBin = Join-Path $releaseBuild 'Release'
$testBin = Join-Path $testBuild 'Release'
foreach ($name in @('BonDriver_Siano.dll', 'BonDriver_PX4.dll')) {
    $dll = Join-Path $releaseBin $name
    $headers = (& dumpbin /nologo /headers $dll | Out-String)
    if ($LASTEXITCODE -ne 0 -or $headers -notmatch '8664 machine \(x64\)') {
        throw "Unexpected architecture in $dll"
    }
    $dependencies = (& dumpbin /nologo /DEPENDENTS $dll | Out-String)
    if ($LASTEXITCODE -ne 0) { throw "dumpbin /DEPENDENTS failed for $dll" }
    $dllNames = [regex]::Matches($dependencies, '(?im)^\s*([A-Za-z0-9_.-]+\.dll)\s*$') |
        ForEach-Object { $_.Groups[1].Value.ToUpperInvariant() }
    $uniqueDependencies = @($dllNames | Sort-Object -Unique)
    if ($uniqueDependencies.Count -ne 1 -or $uniqueDependencies[0] -ne 'KERNEL32.DLL') {
        throw "Unexpected static-CRT imports in ${dll}: $($dllNames -join ', ')"
    }
    Copy-Item -Force $dll (Join-Path $testBin $name)
}

$excluded = 'driver_(siano|px4)_(factory_failure|thread_start_failure|thread_start_cleanup_failure|cleanup_failure)'
Assert-CtestCount $testBuild 31 $excluded
Invoke-Checked 'ctest' @('--test-dir', $testBuild, '-C', 'Release', '--output-on-failure', '-E', $excluded)

$stage = Join-Path $repo '.ci-release-stage/windows-x64'
$config = Join-Path $stage 'config'
New-Item -ItemType Directory -Force -Path $config, (Join-Path $repo 'artifacts') | Out-Null
Copy-Item -Force (Join-Path $releaseBin 'BonDriver_Siano.dll') $stage
Copy-Item -Force (Join-Path $releaseBin 'BonDriver_PX4.dll') $stage
$assets = @(
    'config/BonDriver_Siano.ini.example', 'config/BonDriver_PX4.ini.example',
    'config/channels-ground.example.tsv', 'config/channels-satellite.example.tsv',
    'config/channels-catv.example.tsv', 'README.md', 'docs/release-build.md', 'LICENSE', 'NOTICE'
)
foreach ($asset in $assets) {
    $source = Join-Path $repo $asset
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Required release file missing: $asset" }
    $destination = Join-Path $stage $asset
    New-Item -ItemType Directory -Force -Path (Split-Path $destination) | Out-Null
    Copy-Item -Force $source $destination
}
$sourceSha = if ($env:GITHUB_SHA) { $env:GITHUB_SHA } else { 'unknown' }
$toolset = if ($env:VCToolsVersion) { $env:VCToolsVersion } else { 'Visual Studio 2022 runner toolset' }
$buildInfo = @(
    'Platform: windows-x64'
    'System: Windows Server 2022 x64'
    "Package version: $Version"
    "Source revision: $sourceSha"
    "Compiler: MSVC $toolset"
    'Runtime: static MSVC CRT (/MT)'
)
Set-Content -Encoding ascii -Path (Join-Path $stage 'BUILD-INFO.txt') -Value $buildInfo
$checksumLines = @()
foreach ($asset in @('BonDriver_Siano.dll', 'BonDriver_PX4.dll', 'BUILD-INFO.txt') + $assets) {
    $hash = (Get-FileHash -Algorithm SHA256 (Join-Path $stage $asset)).Hash.ToLowerInvariant()
    $checksumLines += "$hash  $asset"
}
Set-Content -Encoding ascii -Path (Join-Path $stage 'SHA256SUMS') -Value $checksumLines
$archive = Join-Path $repo "artifacts/BonDriver_Userland-$Version-windows-x64.zip"
if (Test-Path $archive) { Remove-Item -Force $archive }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $archive -CompressionLevel Optimal
Write-Host "Created $archive"
