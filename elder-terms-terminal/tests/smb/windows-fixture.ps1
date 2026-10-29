param(
  [Parameter(Mandatory = $true)][ValidateSet('inspect', 'setup', 'verify', 'ui-seed', 'ui-clear', 'cleanup')][string]$Action,
  [string]$RunId = '',
  [string]$ClientAddress = ''
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

function Test-Administrator {
  $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
  $principal = New-Object Security.Principal.WindowsPrincipal($identity)
  return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if ($Action -eq 'inspect') {
  $addresses = @(Get-NetIPConfiguration | Where-Object { $_.NetAdapter.Status -eq 'Up' } |
    ForEach-Object { $_.IPv4Address.IPAddress } |
    Where-Object { $_ -and $_ -notmatch '^(127\.|169\.254\.)' } | Select-Object -Unique)
  @{ administrator = (Test-Administrator); addresses = $addresses } |
    ConvertTo-Json -Compress -Depth 3
  exit 0
}

if ($RunId -notmatch '^[a-f0-9]{24}$') { throw 'Invalid fixture identity' }
if (-not (Test-Administrator)) { throw 'The Windows agent must run as administrator' }

$base = Join-Path $env:ProgramData ('elder-plan12-tests\' + $RunId)
$marker = Join-Path $base 'owner.txt'
$shareRoot = Join-Path $base 'share'
$outside = Join-Path $base 'outside'
$junction = Join-Path $shareRoot 'outside-link'
$user = 'et' + $RunId.Substring(0, 12)
$plainShare = 'et' + $RunId.Substring(0, 10)
$encryptedShare = 'ee' + $RunId.Substring(0, 10)
$readOnlyShare = 'er' + $RunId.Substring(0, 10)
$firewall = 'elder-plan12-smb-' + $RunId
$account = $env:COMPUTERNAME + '\' + $user

if ($Action -eq 'setup') {
  $parsed = $null
  if (-not [Net.IPAddress]::TryParse($ClientAddress, [ref]$parsed) -or
      $parsed.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork) {
    throw 'A single IPv4 client address is required'
  }
  if ([string]::IsNullOrWhiteSpace($env:ELDER_SMB_PASSWORD)) {
    throw 'Missing test password'
  }
  if (Test-Path -LiteralPath $base) { throw 'Fixture path already exists' }
  if (Get-LocalUser -Name $user -ErrorAction SilentlyContinue) { throw 'Fixture account already exists' }
  foreach ($name in @($plainShare, $encryptedShare, $readOnlyShare)) {
    if (Get-SmbShare -Name $name -ErrorAction SilentlyContinue) { throw 'Fixture share already exists' }
  }
  if (Get-NetFirewallRule -Name $firewall -ErrorAction SilentlyContinue) {
    throw 'Fixture firewall rule already exists'
  }

  New-Item -ItemType Directory -Path $base -Force | Out-Null
  Set-Content -LiteralPath $marker -Value $RunId -NoNewline
  New-Item -ItemType Directory -Path $shareRoot -Force | Out-Null
  New-Item -ItemType Directory -Path $outside -Force | Out-Null
  Set-Content -LiteralPath (Join-Path $outside 'sentinel.txt') -Value 'outside sentinel' -NoNewline
  & cmd.exe /d /c ('mklink /J "' + $junction + '" "' + $outside + '"') | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'Could not create the fixture junction' }

  $securePassword = ConvertTo-SecureString $env:ELDER_SMB_PASSWORD -AsPlainText -Force
  New-LocalUser -Name $user -Password $securePassword -PasswordNeverExpires `
    -Description ('elder-plan12-smb-' + $RunId) | Out-Null
  & icacls.exe $shareRoot /grant ('{0}:(OI)(CI)M' -f $account) | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'Could not grant fixture directory access' }
  New-SmbShare -Name $plainShare -Path $shareRoot -FullAccess $account | Out-Null
  New-SmbShare -Name $encryptedShare -Path $shareRoot -FullAccess $account -EncryptData $true | Out-Null
  New-SmbShare -Name $readOnlyShare -Path $shareRoot -ReadAccess $account | Out-Null
  New-NetFirewallRule -Name $firewall -DisplayName $firewall -Direction Inbound -Action Allow `
    -Protocol TCP -LocalPort 445 -RemoteAddress $ClientAddress -Profile Any | Out-Null
  @{ user = $user; plainShare = $plainShare; encryptedShare = $encryptedShare;
    readOnlyShare = $readOnlyShare; reparsePath = 'outside-link' } |
    ConvertTo-Json -Compress
  exit 0
}

if (-not (Test-Path -LiteralPath $marker) -or
    (Get-Content -LiteralPath $marker -Raw) -ne $RunId) {
  if ($Action -eq 'cleanup' -and -not (Test-Path -LiteralPath $base)) {
    @{ cleaned = $true } | ConvertTo-Json -Compress
    exit 0
  }
  throw 'Fixture ownership marker is absent or mismatched'
}

if ($Action -eq 'verify') {
  if ((Get-Content -LiteralPath (Join-Path $outside 'sentinel.txt') -Raw) -ne
      'outside sentinel') { throw 'The outside sentinel changed' }
  $link = Get-Item -LiteralPath $junction -Force
  if ($link.LinkType -ne 'Junction') { throw 'The fixture junction changed' }
  if (@(Get-ChildItem -LiteralPath $shareRoot -Force).Count -ne 1) {
    throw 'Probe files remain in the shared directory'
  }
  @{ sentinelIntact = $true; sharedDirectoryClean = $true } | ConvertTo-Json -Compress
  exit 0
}

if ($Action -eq 'ui-seed') {
  $single = Join-Path $shareRoot 'single.txt'
  $folder = Join-Path $shareRoot 'folder'
  if ((Test-Path -LiteralPath $single) -or (Test-Path -LiteralPath $folder)) {
    throw 'SMB UI fixture files already exist'
  }
  [IO.File]::WriteAllText($single, "single SMB file`n")
  New-Item -ItemType Directory -Path $folder | Out-Null
  [IO.File]::WriteAllText((Join-Path $folder 'nested.txt'), "nested SMB file`n")
  @{ seeded = $true } | ConvertTo-Json -Compress
  exit 0
}

if ($Action -eq 'ui-clear') {
  $single = Join-Path $shareRoot 'single.txt'
  $folder = Join-Path $shareRoot 'folder'
  if ([IO.File]::ReadAllText($single) -ne "single SMB file`n" -or
      [IO.File]::ReadAllText((Join-Path $folder 'nested.txt')) -ne "nested SMB file`n") {
    throw 'SMB UI fixture content changed'
  }
  Remove-Item -LiteralPath $single -Force
  Remove-Item -LiteralPath $folder -Recurse -Force
  @{ cleared = $true } | ConvertTo-Json -Compress
  exit 0
}

foreach ($name in @($plainShare, $encryptedShare, $readOnlyShare)) {
  $existing = Get-SmbShare -Name $name -ErrorAction SilentlyContinue
  if ($existing) {
    if ($existing.Path -ne $shareRoot) { throw 'A share name was reused by another path' }
    Remove-SmbShare -Name $name -Force | Out-Null
  }
}
if (Get-NetFirewallRule -Name $firewall -ErrorAction SilentlyContinue) {
  Remove-NetFirewallRule -Name $firewall | Out-Null
}
$existingUser = Get-LocalUser -Name $user -ErrorAction SilentlyContinue
if ($existingUser) {
  if ($existingUser.Description -ne ('elder-plan12-smb-' + $RunId)) {
    throw 'The fixture account ownership changed'
  }
  Remove-LocalUser -Name $user
}
if (Test-Path -LiteralPath $junction) {
  & cmd.exe /d /c ('rmdir "' + $junction + '"') | Out-Null
  if ($LASTEXITCODE -ne 0 -or (Test-Path -LiteralPath $junction)) {
    throw 'Could not remove the fixture junction safely'
  }
}
$sentinel = Join-Path $outside 'sentinel.txt'
if ((Test-Path -LiteralPath $sentinel) -and
    (Get-Content -LiteralPath $sentinel -Raw) -ne 'outside sentinel') {
  throw 'The outside sentinel changed during cleanup'
}
Remove-Item -LiteralPath $base -Recurse -Force
@{ cleaned = $true } | ConvertTo-Json -Compress
