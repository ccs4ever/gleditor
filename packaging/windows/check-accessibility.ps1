param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$Sample
)

$ErrorActionPreference = 'Stop'
$framework = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\WPF'
Add-Type -Path (Join-Path $framework 'UIAutomationTypes.dll')
Add-Type -Path (Join-Path $framework 'UIAutomationClient.dll')
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class AccessibilityWindows {
    public delegate bool Callback(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")]
    public static extern bool EnumWindows(Callback callback, IntPtr parameter);
    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
}
'@

$Executable = (Resolve-Path $Executable).Path
$Sample = (Resolve-Path $Sample).Path
$scratch = Join-Path $env:RUNNER_TEMP ('gleditor-uia-' + [Guid]::NewGuid())
New-Item -ItemType Directory -Path $scratch | Out-Null
$env:XDG_DATA_HOME = Join-Path $scratch 'data'
$env:XDG_CONFIG_HOME = Join-Path $scratch 'config'
$env:SDL_AUDIODRIVER = 'dummy'
$env:SDL_VIDEODRIVER = 'windows'
$env:GALLIUM_DRIVER = 'llvmpipe'
$env:LIBGL_ALWAYS_SOFTWARE = '1'
$store = Join-Path $scratch 'store'
$stdout = Join-Path $scratch 'stdout.log'
$stderr = Join-Path $scratch 'stderr.log'
$arguments = '--backend opengl --no-present --import "{0}" "{1}"' -f $Sample, $store
$application = Start-Process -FilePath $Executable -ArgumentList $arguments -PassThru `
    -WorkingDirectory (Split-Path $Executable) `
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr

try {
    $deadline = (Get-Date).AddSeconds(60)
    do {
        if ($application.HasExited) {
            throw "Application exited before UI Automation readback (exit $($application.ExitCode))."
        }
        # MainWindowHandle excludes hidden windows. Enumerate the process's
        # HWNDs so the check also works when --no-present keeps SDL hidden.
        $handles = New-Object 'System.Collections.Generic.List[IntPtr]'
        $callback = [AccessibilityWindows+Callback] {
            param($window, $parameter)
            [uint32]$owner = 0
            [AccessibilityWindows]::GetWindowThreadProcessId($window, [ref]$owner) | Out-Null
            if ($owner -eq $application.Id) { $handles.Add($window) }
            return $true
        }
        [AccessibilityWindows]::EnumWindows($callback, [IntPtr]::Zero) | Out-Null
        foreach ($window in $handles) {
            $root = [System.Windows.Automation.AutomationElement]::FromHandle($window)
            if ($null -eq $root -or $root.Current.Name -ne 'Xuzz') { continue }
            $elements = $root.FindAll(
                [System.Windows.Automation.TreeScope]::Descendants,
                [System.Windows.Automation.Condition]::TrueCondition)
            foreach ($element in $elements) {
                $pattern = $null
                if ($element.TryGetCurrentPattern(
                    [System.Windows.Automation.TextPattern]::Pattern, [ref]$pattern)) {
                    $text = $pattern.DocumentRange.GetText(-1)
                    if ($text.Contains('The quick brown fox')) {
                        Write-Output "UI Automation read back document text through TextPattern ($($elements.Count) elements)."
                        exit 0
                    }
                }
            }
        }
        Start-Sleep -Milliseconds 250
    } while ((Get-Date) -lt $deadline)
    throw 'UI Automation did not expose the imported document through TextPattern within 60 seconds.'
} finally {
    if (-not $application.HasExited) { Stop-Process -Id $application.Id -Force }
    $application.Dispose()
}
