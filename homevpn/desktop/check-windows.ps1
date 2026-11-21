# Installs the Windows installer of a published HomeVPN release on this machine and checks what a person sees:
# shortcut names, the icon of the executable, the window title. Pictures and lists go to the report folder.
# Usage: check-windows.ps1 -Version 5.1.1.1109 -Repository owner/repo -Report check
param(
    [Parameter(Mandatory)] [string] $Version,
    [Parameter(Mandatory)] [string] $Repository,
    [Parameter(Mandatory)] [string] $Report
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing, System.Windows.Forms

$report = (New-Item -ItemType Directory -Force $Report).FullName
$brand = Get-Content -Raw -Encoding utf8 (Join-Path $PSScriptRoot 'brand.cmake')
$expectedName = [regex]::Match($brand, 'CLIENT_DISPLAY_NAME "([^"]+)"').Groups[1].Value
$installDir = Join-Path $env:ProgramFiles 'AmneziaVPN'
$executable = Join-Path $installDir 'AmneziaVPN.exe'
$failures = @()

function Save-Screen([string] $name) {
    $bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $bitmap = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($bounds.Location, [System.Drawing.Point]::Empty, $bounds.Size)
    $bitmap.Save((Join-Path $report $name))
}

$download = New-Item -ItemType Directory -Force (Join-Path $env:RUNNER_TEMP 'homevpn-installer')
gh release download "homevpn-$Version" --repo $Repository --pattern '*_windows_x64.exe' --dir $download.FullName
$installer = (Get-ChildItem $download.FullName -Filter '*.exe' | Select-Object -First 1).FullName

$arguments = 'install', '--accept-licenses', '--accept-messages', '--confirm-command'
$process = Start-Process $installer -ArgumentList $arguments -PassThru `
    -RedirectStandardOutput (Join-Path $report 'install.log') -RedirectStandardError (Join-Path $report 'install-errors.log')
if (-not $process.WaitForExit(600000)) {
    Save-Screen 'install-hangs.png'
    $process.Kill()
    throw 'the installer did not finish in 10 minutes'
}
if ($process.ExitCode -ne 0) { throw "the installer exited with code $($process.ExitCode)" }
if (-not (Test-Path $executable)) { throw "no $executable after the installation" }

$shell = New-Object -ComObject Shell.Application
$folders = [Environment]::GetFolderPath('Desktop'), [Environment]::GetFolderPath('CommonDesktopDirectory'),
           [Environment]::GetFolderPath('Programs'), [Environment]::GetFolderPath('CommonPrograms')
$shortcuts = Get-ChildItem $folders -Filter '*.lnk' -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $shell.Namespace($_.DirectoryName).ParseName($_.Name).GetLink.Path -eq $executable }
$shortcuts | ForEach-Object { $_.FullName } | Set-Content -Encoding utf8 (Join-Path $report 'shortcuts.txt')
if ($shortcuts.Count -lt 2) { $failures += "expected a desktop and a start menu shortcut, found $($shortcuts.Count)" }
$shortcuts | Where-Object { $_.BaseName -ne $expectedName } |
    ForEach-Object { $failures += "the shortcut $($_.FullName) is not named $expectedName" }

[System.Drawing.Icon]::ExtractAssociatedIcon($executable).ToBitmap().Save((Join-Path $report 'executable-icon.png'))
(Get-Item $executable).VersionInfo | Format-List | Out-File -Encoding utf8 (Join-Path $report 'executable-version.txt')

(New-Object -ComObject Shell.Application).MinimizeAll()
Start-Sleep -Seconds 3
Save-Screen 'desktop.png'

Start-Process $executable
Start-Sleep -Seconds 30
$windows = Get-Process | Where-Object { $_.Path -eq $executable -and $_.MainWindowTitle }
$windows | ForEach-Object { $_.MainWindowTitle } | Set-Content -Encoding utf8 (Join-Path $report 'window-titles.txt')
Save-Screen 'application.png'
if (-not $windows) { $failures += 'the application did not show a window in 30 seconds' }
$windows | Where-Object { $_.MainWindowTitle -ne $expectedName } |
    ForEach-Object { $failures += "the window is titled $($_.MainWindowTitle), not $expectedName" }

$failures | Set-Content -Encoding utf8 (Join-Path $report 'failures.txt')
if ($failures) {
    $failures | ForEach-Object { Write-Host "::error::$_" }
    exit 1
}
Write-Host "The shortcuts and the window are named $expectedName"
