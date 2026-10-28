param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [string]$Document,
    [int]$TimeoutSeconds = 20
)

$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class HpsStartupWindows {
    public delegate bool Callback(IntPtr window, IntPtr state);
    [DllImport("user32.dll")] public static extern bool EnumWindows(Callback callback, IntPtr state);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, Callback callback, IntPtr state);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr window, StringBuilder text, int length);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr window, StringBuilder text, int length);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr window, uint message, UIntPtr wp, IntPtr lp, uint flags, uint timeout, out UIntPtr result);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, UIntPtr wp, IntPtr lp);
}
'@

function Find-DocumentWindow([int]$ProcessId, [string]$ExpectedTitle) {
    $script:readyWindow = [IntPtr]::Zero
    [void][HpsStartupWindows]::EnumWindows({ param($window, $state)
        [uint32]$ownerId = 0
        [void][HpsStartupWindows]::GetWindowThreadProcessId($window, [ref]$ownerId)
        if ($ownerId -eq $ProcessId) {
            $className = New-Object Text.StringBuilder 256
            [void][HpsStartupWindows]::GetClassName($window, $className, 256)
            if ($className.ToString().StartsWith('Afx:')) {
                $script:hasDocument = $false
                [void][HpsStartupWindows]::EnumChildWindows($window, { param($child, $unused)
                    $title = New-Object Text.StringBuilder 512
                    [void][HpsStartupWindows]::GetWindowText($child, $title, 512)
                    if ($title.ToString().Contains($ExpectedTitle)) { $script:hasDocument = $true }
                    return $true
                }, [IntPtr]::Zero)
                [UIntPtr]$result = [UIntPtr]::Zero
                if ($script:hasDocument -and [HpsStartupWindows]::SendMessageTimeout(
                    $window, 0, [UIntPtr]::Zero, [IntPtr]::Zero, 2, 300, [ref]$result) -ne [IntPtr]::Zero) {
                    $script:readyWindow = $window
                }
            }
        }
        return $true
    }, [IntPtr]::Zero)
    return $script:readyWindow
}

$executablePath = (Resolve-Path -LiteralPath $Executable).Path
$startOptions = @{ FilePath = $executablePath; WindowStyle = 'Hidden'; PassThru = $true }
$expectedTitle = 'HPS'
$originalHash = $null
if ($Document) {
    $documentPath = (Resolve-Path -LiteralPath $Document).Path
    $startOptions.ArgumentList = '"' + $documentPath + '"'
    $expectedTitle = [IO.Path]::GetFileNameWithoutExtension($documentPath)
    $originalHash = (Get-FileHash -LiteralPath $documentPath).Hash
}
$appProcess = Start-Process @startOptions
# Retain the process handle so ExitCode remains available after normal shutdown.
$null = $appProcess.Handle
$deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
$readyCount = 0
do {
    if ($appProcess.HasExited) { throw "Application exited during startup: $($appProcess.ExitCode)" }
    $frame = Find-DocumentWindow $appProcess.Id $expectedTitle
    if ($frame -ne [IntPtr]::Zero) { $readyCount++ } else { $readyCount = 0 }
    if ($readyCount -ge 3) { break }
    Start-Sleep -Milliseconds 250
} while ([DateTime]::UtcNow -lt $deadline)
if ($readyCount -lt 3) {
    throw "Document '$expectedTitle' did not become responsive within $TimeoutSeconds seconds (PID $($appProcess.Id))."
}
if (-not [HpsStartupWindows]::PostMessage($frame, 0x0010, [UIntPtr]::Zero, [IntPtr]::Zero)) {
    throw 'Could not request normal application shutdown.'
}
if (-not $appProcess.WaitForExit(10000)) { throw "Normal shutdown timed out (PID $($appProcess.Id))." }
if ($appProcess.ExitCode -ne 0) { throw "Application exit code: $($appProcess.ExitCode)" }
if ($Document -and (Get-FileHash -LiteralPath $documentPath).Hash -ne $originalHash) {
    throw 'Opening and closing changed the reference document.'
}
Write-Output "PASS: $expectedTitle opened, responded and closed normally without saving."
