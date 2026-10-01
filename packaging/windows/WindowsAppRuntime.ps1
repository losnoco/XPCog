# The Windows App Runtime, for the WinUI player: whether it is installed, and
# fetching Microsoft's installer for it when it is not.
#
# Run by the XPCog installer from its plugins directory:
#
#   WindowsAppRuntime.ps1 -Check -Minimum 2.5.1.0
#       exit 0 when the x64 framework package Microsoft.WindowsAppRuntime.2 is
#       installed at that version or later, 1 when it is not.
#
#   WindowsAppRuntime.ps1 -Download -Url <url> -Sha256 <hash> -OutFile <path>
#       exit 0 with the file at <path> and its hash checked, 2 when the
#       download failed, 3 when what arrived was not that file.
#
# A script rather than a command line in the .nsi, because NSIS reads $ as its
# own variables and PowerShell is made of them.

[CmdletBinding()]
param(
    [switch] $Check,
    [string] $Minimum,
    [switch] $Download,
    [string] $Url,
    [string] $Sha256,
    [string] $OutFile
)

$ErrorActionPreference = 'Stop'

if ($Check) {
    # The framework package the bootstrapper looks for: from 2.0 one package
    # per major version, versioned with the release itself (2.5.1.0).
    $wanted = [version] $Minimum
    $found = Get-AppxPackage -Name 'Microsoft.WindowsAppRuntime.2' -ErrorAction SilentlyContinue |
        Where-Object { $_.Architecture -eq 'X64' -and [version] $_.Version -ge $wanted }
    if ($found) { exit 0 } else { exit 1 }
}

if ($Download) {
    # Invoke-WebRequest's progress bar slows a large download by an order of
    # magnitude in Windows PowerShell 5.1, which is what the installer runs.
    $ProgressPreference = 'SilentlyContinue'
    try {
        Invoke-WebRequest -Uri $Url -OutFile $OutFile -UseBasicParsing
    } catch {
        exit 2
    }
    if ((Get-FileHash -Algorithm SHA256 -Path $OutFile).Hash -ne $Sha256.ToUpperInvariant()) {
        Remove-Item -Force $OutFile
        exit 3
    }
    exit 0
}

exit 64
