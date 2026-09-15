param(
  [Parameter(Mandatory = $true)]
  [string]$PakPath,
  [string]$OutputDir = ".\TakaroConan-artifact",
  [string]$BuildReportPath = ".\BUILD_REPORT.md",
  [string]$SourceEvidencePath = ".\SOURCE_EVIDENCE.md",
  [Parameter(Mandatory = $true)]
  [string]$DevKitVersion,
  [string]$DevKitBranch = "enhanced",
  [string]$DevKitRoot = "<redacted>",
  [string]$SourceRevision = "",
  [switch]$CompiledInDevKit,
  [switch]$CookedInDevKit
)

$ErrorActionPreference = "Stop"
$failures = 0

function Fail($message) {
  Write-Host "FAIL: $message" -ForegroundColor Red
  $script:failures += 1
}

function Ok($message) {
  Write-Host "OK: $message"
}

function Test-OutputDirReady($path) {
  if (-not (Test-Path -LiteralPath $path)) {
    Ok "output directory does not exist yet: $path"
    return
  }

  $children = Get-ChildItem -LiteralPath $path -Force -ErrorAction SilentlyContinue
  if ($children.Count -eq 0) {
    Ok "output directory is empty: $path"
  } else {
    Fail "output directory is not empty; use a fresh folder so stale files cannot enter the returned runtime bundle: $path"
  }
}

function Test-ExactRuntimeBundle($path) {
  $expected = @(
    "TakaroConan.pak",
    "artifact-manifest.json",
    "BUILD_REPORT.md",
    "SOURCE_EVIDENCE.md"
  )

  $files = Get-ChildItem -LiteralPath $path -File -Force | Select-Object -ExpandProperty Name | Sort-Object
  $expectedSorted = $expected | Sort-Object
  $unexpected = @($files | Where-Object { $_ -notin $expectedSorted })
  $missing = @($expectedSorted | Where-Object { $_ -notin $files })

  if ($unexpected.Count -eq 0 -and $missing.Count -eq 0 -and $files.Count -eq $expectedSorted.Count) {
    Ok "returned runtime bundle contains exactly the four required files"
  } else {
    if ($missing.Count -gt 0) {
      Fail "returned runtime bundle is missing required files: $($missing -join ', ')"
    }
    if ($unexpected.Count -gt 0) {
      Fail "returned runtime bundle contains unexpected files: $($unexpected -join ', ')"
    }
  }
}

function Test-BuildReport($path, $expectedSha, $expectedSize) {
  if (-not (Test-Path -LiteralPath $path)) {
    Fail "build report missing: $path"
    return
  }

  $report = Get-Content -LiteralPath $path -Raw
  $requiredPatterns = @(
    "Active mod name:\s*`?TakaroConan`?",
    "TakaroConan_ModController",
    "BP_TakaroBridgeComponent",
    "BP_TakaroClientMarkerComponent",
    "Implementation plan:\s*IMPLEMENTATION_PLAN\.md",
    "Source contract:\s*BUILD_SOURCE_CONTRACT\.json",
    "Bridge contract smoke result:\s*\S",
    "BRIDGE_CONTRACT_SMOKE\.ps1",
    "server-wide /mod/result|server-wide command result",
    "targeted /mod/result|targeted command result",
    "inbound /mod/event|inbound chat event",
    "nested result payload|nested /mod/result",
    "Bridge component runs only on server authority",
    "Client-side assets contain no Takaro cloud tokens",
    "Runtime source name:\s*TakaroConan",
    "Bridge base URL:\s*http://127\.0\.0\.1:3010",
    "Poll endpoint:\s*/mod/poll",
    "Result endpoint:\s*/mod/result",
    "Event endpoint:\s*/mod/event",
    "Source evidence report:\s*SOURCE_EVIDENCE\.md",
    "Polling uses `/mod/poll(\?source=TakaroConan)?`[\s\S]*?(source=TakaroConan|X-Takaro-Mod-Source:\s*TakaroConan)",
    "Results post to `/mod/result` with[\s\S]*?(source=TakaroConan|X-Takaro-Mod-Source:\s*TakaroConan)",
    "Inbound chat posts to `/mod/event` with[\s\S]*?(source=TakaroConan|X-Takaro-Mod-Source:\s*TakaroConan)",
    "does not rely on .*User-Agent|User-Agent.*source attribution",
    "/mod/poll",
    "/mod/result",
    "/mod/event",
    "Server-wide messages render in normal Conan chat",
    "Targeted messages render to the target player",
    "Inbound player chat includes stable Steam/platform identity",
    "No Pippi/Amunet assets or RCON chat commands are referenced",
    "IMPLEMENTATION_PLAN\.md",
    "SOURCE_EVIDENCE\.md",
    "Intake validator passed"
  )

  foreach ($pattern in $requiredPatterns) {
    if ($report -match $pattern) {
      Ok "build report contains required evidence: $pattern"
    } else {
      Fail "build report missing required evidence: $pattern"
    }
  }

  if ($report -match "(?m)- \[ \]") {
    Fail "build report still contains unchecked checklist items"
  } else {
    Ok "build report has no unchecked checklist items"
  }

  $placeholderPattern = "(?mi)^- (Build host|Builder|Built at UTC|DevKit distribution|DevKit branch|DevKit version|Game branch/version|Source workspace or commit|Implementation plan|Source contract|Bridge contract smoke result|Active mod folder|ModController asset|Bridge component asset|Client marker component asset|Bridge component attach target|Component copy rule|Runtime source name|Bridge base URL|Poll endpoint|Result endpoint|Event endpoint|Source evidence report|Cook status|Output folder|Artifact path|Artifact size|SHA-256|DevKit output log excerpt path):\s*(.*<[^>]+>.*|TBD|TODO|fill.*|unknown)\s*$"
  if ($report -match $placeholderPattern) {
    Fail "build report contains placeholder field values"
  } else {
    Ok "build report contains no placeholder field values"
  }

  if (-not [string]::IsNullOrWhiteSpace($expectedSha)) {
    if ($report -match [regex]::Escape($expectedSha)) {
      Ok "build report contains exact artifact SHA-256"
    } else {
      Fail "build report missing exact artifact SHA-256: $expectedSha"
    }
  }

  if ($null -ne $expectedSize -and $expectedSize -gt 0) {
    $sizePattern = "(Artifact size:\s*$expectedSize(\s|$)|$expectedSize\s*bytes)"
    if ($report -match $sizePattern) {
      Ok "build report contains exact artifact size"
    } else {
      Fail "build report missing exact artifact size: $expectedSize"
    }
  }

  if ($report -match "TAKARO_(TOKEN|API_KEY|REGISTRATION)\s*[:=]\s*[""']?[A-Za-z0-9_-]{12,}|(registrationToken|rconPassword|RCON_PASSWORD)\s*[:=]\s*[""']?[A-Za-z0-9_-]{12,}|Authorization:\s*Bearer\s+[A-Za-z0-9._-]{12,}|eyJ[A-Za-z0-9_-]{20,}\.") {
    Fail "build report contains an obvious secret/token marker"
  } else {
    Ok "build report contains no obvious secret/token markers"
  }
}

function Test-SourceEvidence($path, $expectedSha) {
  if (-not (Test-Path -LiteralPath $path)) {
    Fail "source evidence missing: $path"
    return
  }

  $evidence = Get-Content -LiteralPath $path -Raw
  $requiredPatterns = @(
    "TakaroConan_ModController",
    "BP_TakaroBridgeComponent",
    "BP_TakaroClientMarkerComponent",
    "Implementation plan:\s*IMPLEMENTATION_PLAN\.md",
    "Source contract:\s*BUILD_SOURCE_CONTRACT\.json",
    "Bridge contract smoke result:\s*\S",
    "BRIDGE_CONTRACT_SMOKE\.ps1",
    "bridge smoke result includes server-wide command result|server-wide command result",
    "targeted command result",
    "inbound chat event proof|inbound chat event",
    "server authority|server-only|authority only",
    "polls `/mod/poll` with[\s\S]*?(X-Takaro-Mod-Source:\s*TakaroConan|source=TakaroConan)",
    "command results to `/mod/result` with[\s\S]*?(X-Takaro-Mod-Source:\s*TakaroConan|source=TakaroConan)",
    "inbound player chat to `/mod/event` with[\s\S]*?(X-Takaro-Mod-Source:\s*TakaroConan|source=TakaroConan)",
    "does not rely on .*User-Agent|User-Agent.*source attribution",
    "/mod/poll",
    "/mod/result",
    "/mod/event",
    "Server-wide Takaro messages render|server-wide Takaro messages render",
    "Targeted Takaro messages render|targeted Takaro messages render",
    "Inbound player chat includes stable Steam/platform identity",
    "does not call `/mod/poll`",
    "does not call `/mod/result`",
    "does not call `/mod/event`",
    "does not reference Pippi assets",
    "does not use Pippi RCON chat commands",
    "Implementation plan evidence",
    "IMPLEMENTATION_PLAN\.md",
    "Compile status:\s*(passed|success|compiled)",
    "Cook status:\s*(passed|success|cooked)",
    "TakaroConan\.pak"
  )

  foreach ($pattern in $requiredPatterns) {
    if ($evidence -match $pattern) {
      Ok "source evidence contains required evidence: $pattern"
    } else {
      Fail "source evidence missing required evidence: $pattern"
    }
  }

  if ($evidence -match "(?m)- \[ \]") {
    Fail "source evidence still contains unchecked checklist items"
  } else {
    Ok "source evidence has no unchecked checklist items"
  }

  $placeholderPattern = "(?mi)^- (Build host|Builder|Captured at UTC|DevKit distribution|DevKit branch|DevKit version|Active mod folder|Source workspace or commit|Implementation plan|Source contract|Bridge contract smoke result|ModController asset path|Bridge component asset path|Client marker component asset path|Bridge component attach target|Component copy rule|Evidence files or screenshots folder|Implementation plan evidence|Source contract evidence|Compile status|Cook status|Cooked artifact path|DevKit output log excerpt path|SHA-256 of returned `TakaroConan\.pak`):\s*(.*<[^>]+>.*|TBD|TODO|fill.*|unknown)\s*$"
  if ($evidence -match $placeholderPattern) {
    Fail "source evidence contains placeholder field values"
  } else {
    Ok "source evidence contains no placeholder field values"
  }

  if (-not [string]::IsNullOrWhiteSpace($expectedSha)) {
    if ($evidence -match [regex]::Escape($expectedSha)) {
      Ok "source evidence contains exact artifact SHA-256"
    } else {
      Fail "source evidence missing exact artifact SHA-256: $expectedSha"
    }
  }

  if ($evidence -match "TAKARO_(TOKEN|API_KEY|REGISTRATION)\s*[:=]\s*[""']?[A-Za-z0-9_-]{12,}|(registrationToken|rconPassword|RCON_PASSWORD)\s*[:=]\s*[""']?[A-Za-z0-9_-]{12,}|Authorization:\s*Bearer\s+[A-Za-z0-9._-]{12,}|eyJ[A-Za-z0-9_-]{20,}\.") {
    Fail "source evidence contains an obvious secret/token marker"
  } else {
    Ok "source evidence contains no obvious secret/token markers"
  }
}

$pakHash = ""
$pakSize = 0

if (-not (Test-Path -LiteralPath $PakPath)) {
  Fail "pak not found: $PakPath"
} elseif ((Split-Path -Leaf $PakPath) -ne "TakaroConan.pak") {
  Fail "pak must be named TakaroConan.pak"
} else {
  Ok "pak found: $PakPath"
  $pakHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $PakPath).Hash.ToLowerInvariant()
  $pakSize = (Get-Item -LiteralPath $PakPath).Length
  Ok "pak SHA-256 computed"
  Ok "pak size computed: $pakSize bytes"
}

if (-not $CompiledInDevKit) {
  Fail "pass -CompiledInDevKit only after confirming the TakaroConan mod compiles in the Conan DevKit"
}

if (-not $CookedInDevKit) {
  Fail "pass -CookedInDevKit only after confirming the TakaroConan mod cooks/packages in the Conan DevKit"
}

if ([string]::IsNullOrWhiteSpace($SourceRevision)) {
  try {
    $SourceRevision = (git rev-parse --short HEAD 2>$null)
  } catch {
    $SourceRevision = "workspace-snapshot"
  }
  if ([string]::IsNullOrWhiteSpace($SourceRevision)) {
    $SourceRevision = "workspace-snapshot"
  }
}

Test-BuildReport $BuildReportPath $pakHash $pakSize
Test-SourceEvidence $SourceEvidencePath $pakHash
Test-OutputDirReady $OutputDir

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

if ($failures -eq 0) {
  $targetPak = Join-Path $OutputDir "TakaroConan.pak"
  Copy-Item -LiteralPath $PakPath -Destination $targetPak -Force
  $hash = Get-FileHash -Algorithm SHA256 -LiteralPath $targetPak
  $size = (Get-Item -LiteralPath $targetPak).Length

  if ($hash.Hash.ToLowerInvariant() -ne $pakHash) {
    Fail "copied pak SHA-256 changed during collection"
  }

  if ($size -ne $pakSize) {
    Fail "copied pak size changed during collection"
  }

  $manifest = [ordered]@{
    artifact = "TakaroConan.pak"
    sha256 = $hash.Hash.ToLowerInvariant()
    sizeBytes = $size
    builtAt = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    buildHost = $env:COMPUTERNAME
    buildReport = "BUILD_REPORT.md"
    devkit = [ordered]@{
      distribution = "Conan Exiles Enhanced DevKit"
      branch = $DevKitBranch
      version = $DevKitVersion
      installPath = $DevKitRoot
    }
    source = [ordered]@{
      repo = "gettakaro/connectors"
      commit = $SourceRevision
      handoffPath = "conan-exiles/mod/TakaroConanBridge/devkit-handoff"
      modName = "TakaroConan"
      implementationPlan = "IMPLEMENTATION_PLAN.md"
      sourceContract = "BUILD_SOURCE_CONTRACT.json"
      sourceEvidence = "SOURCE_EVIDENCE.md"
    }
    sourceEvidence = [ordered]@{
      report = "SOURCE_EVIDENCE.md"
      sourceContract = "BUILD_SOURCE_CONTRACT.json"
      devkitSourceReturned = $true
      modControllerDocumented = $true
      serverBridgeAssetDocumented = $true
      clientMarkerAssetDocumented = $true
      serverAuthorityPollingDocumented = $true
      explicitSourceAttributionDocumented = $true
      noUserAgentSourceAttributionDocumented = $true
      stableInboundIdentityDocumented = $true
      bridgeContractSmokeDocumented = $true
      clientMarkerNoPollingDocumented = $true
      noPippiReferencesDocumented = $true
    }
    assets = [ordered]@{
      modController = "Content/Mods/TakaroConan/TakaroConan_ModController"
      bridgeComponent = "Content/Mods/TakaroConan/BP_TakaroBridgeComponent"
      clientMarkerComponent = "Content/Mods/TakaroConan/BP_TakaroClientMarkerComponent"
    }
    runtimeContract = [ordered]@{
      sourceName = "TakaroConan"
      bridgeBaseUrl = "http://127.0.0.1:3010"
      serverAuthorityPollingOnly = $true
      clientMarkerNoSecrets = $true
      clientMarkerDoesNotPoll = $true
      explicitSourceAttributionRequired = $true
      userAgentSourceAttributionAllowed = $false
      inboundChatRequiresStableIdentity = $true
      pollEndpoint = "/mod/poll"
      resultEndpoint = "/mod/result"
      eventEndpoint = "/mod/event"
      supportsServerWideChat = $true
      supportsTargetedChat = $true
      supportsInboundChat = $true
      usesPippiRconChatCommands = $false
    }
    security = [ordered]@{
      containsTakaroCloudToken = $false
      containsRegistrationToken = $false
      containsRconPassword = $false
      containsPippiAssets = $false
    }
    validation = [ordered]@{
      compiledInDevKit = [bool]$CompiledInDevKit
      cookedInDevKit = [bool]$CookedInDevKit
      validateTakaroPakPassed = $false
      installedOnServer = $false
      installedOnClient = $false
      liveAuditPassed = $false
    }
  }
  $manifest | ConvertTo-Json -Depth 5 | Set-Content -Encoding UTF8 -Path (Join-Path $OutputDir "artifact-manifest.json")
  Ok "wrote artifact manifest"

  Copy-Item -LiteralPath $BuildReportPath -Destination (Join-Path $OutputDir "BUILD_REPORT.md") -Force
  Ok "copied build report"

  Copy-Item -LiteralPath $SourceEvidencePath -Destination (Join-Path $OutputDir "SOURCE_EVIDENCE.md") -Force
  Ok "copied source evidence"

  Test-ExactRuntimeBundle $OutputDir
}

if ($failures -gt 0) {
  Write-Host ""
  Write-Host "TakaroConan artifact collection failed ($failures failure(s))." -ForegroundColor Red
  exit 1
}

Write-Host ""
Write-Host "TakaroConan artifact collection ready: $OutputDir"
