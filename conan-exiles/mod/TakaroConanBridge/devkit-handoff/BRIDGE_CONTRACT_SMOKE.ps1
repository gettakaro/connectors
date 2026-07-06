param(
  [int]$Port = 3010,
  [int]$DurationSeconds = 180,
  [string]$OutputPath = ".\BRIDGE_CONTRACT_SMOKE.result.json",
  [string]$ExpectedPlayerId = "76561198000000000",
  [string]$ExpectedPlayerName = "TakaroDevkitTester"
)

$ErrorActionPreference = "Stop"

$marker = "TAKARO_CONAN_DEVKIT_SMOKE_$([DateTimeOffset]::UtcNow.ToUnixTimeSeconds())"
$prefix = "http://127.0.0.1:$Port/"
$deadline = [DateTime]::UtcNow.AddSeconds($DurationSeconds)
$failures = New-Object System.Collections.Generic.List[string]
$requests = New-Object System.Collections.Generic.List[object]
$results = New-Object System.Collections.Generic.List[object]
$events = New-Object System.Collections.Generic.List[object]

$commands = New-Object System.Collections.Queue
$commands.Enqueue([ordered]@{
  requestId = "$marker-server-wide"
  action = "sendMessage"
  args = [ordered]@{
    message = "$marker server-wide"
    senderNameOverride = "Takaro"
  }
})
$commands.Enqueue([ordered]@{
  requestId = "$marker-targeted"
  action = "sendMessage"
  args = [ordered]@{
    message = "$marker targeted"
    senderNameOverride = "Takaro"
    recipient = [ordered]@{
      gameId = $ExpectedPlayerId
      platformId = "steam:$ExpectedPlayerId"
      name = $ExpectedPlayerName
    }
  }
})

function Add-Failure($message) {
  Write-Host "FAIL: $message" -ForegroundColor Red
  $script:failures.Add($message) | Out-Null
}

function Write-JsonResponse($context, $statusCode, $payload) {
  $json = $payload | ConvertTo-Json -Depth 20 -Compress
  $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
  $context.Response.StatusCode = $statusCode
  $context.Response.ContentType = "application/json"
  $context.Response.ContentLength64 = $bytes.Length
  $context.Response.OutputStream.Write($bytes, 0, $bytes.Length)
  $context.Response.OutputStream.Close()
}

function Read-JsonBody($request) {
  $reader = New-Object System.IO.StreamReader($request.InputStream, $request.ContentEncoding)
  $raw = $reader.ReadToEnd()
  if ([string]::IsNullOrWhiteSpace($raw)) {
    return $null
  }
  return $raw | ConvertFrom-Json
}

function Test-Source($request) {
  $querySource = $request.QueryString["source"]
  $headerSource = $request.Headers["X-Takaro-Mod-Source"]
  return ($querySource -match "^TakaroConan($|[ /:@+-])") -or ($headerSource -match "^TakaroConan($|[ /:@+-])")
}

function Test-SteamIdentity($player) {
  if ($null -eq $player) {
    return $false
  }
  $ids = @($player.gameId, $player.platformId, $player.steamId) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
  foreach ($id in $ids) {
    if ($id -match "^(steam:)?[0-9]{17}$") {
      return $true
    }
  }
  return $false
}

function Save-Receipt($path, $status) {
  $receipt = [ordered]@{
    status = $status
    marker = $script:marker
    startedBridgeUrl = $script:prefix
    durationSeconds = $DurationSeconds
    requiredSource = "TakaroConan"
    expectedServerWideMessage = "$script:marker server-wide"
    expectedTargetedMessage = "$script:marker targeted"
    expectedInboundMessage = "$script:marker inbound"
    resultsSeen = $script:results
    eventsSeen = $script:events
    requestsSeen = $script:requests
    failures = $script:failures
  }
  $receipt | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $path -Encoding UTF8
}

$listener = [System.Net.HttpListener]::new()
$listener.Prefixes.Add($prefix)

try {
  $listener.Start()
} catch {
  Add-Failure "could not listen on $prefix. Close the real sidecar or run from an elevated shell if Windows URL ACL blocks HttpListener: $($_.Exception.Message)"
  Save-Receipt $OutputPath "failed-start"
  exit 1
}

Write-Host "TakaroConan DevKit bridge contract smoke listening on $prefix"
Write-Host "Configure BP_TakaroBridgeComponent BridgeBaseUrl to http://127.0.0.1:$Port"
Write-Host "Expected player inbound chat marker: $marker inbound"
Write-Host "Stop the DevKit play session after this script reports pass/fail."

try {
  while ([DateTime]::UtcNow -lt $deadline) {
    $async = $listener.BeginGetContext($null, $null)
    while (-not $async.AsyncWaitHandle.WaitOne(200)) {
      if ([DateTime]::UtcNow -ge $deadline) {
        break
      }
    }
    if (-not $async.IsCompleted) {
      continue
    }

    $context = $listener.EndGetContext($async)
    $request = $context.Request
    $path = $request.Url.AbsolutePath
    $requests.Add([ordered]@{
      method = $request.HttpMethod
      path = $path
      hasQuerySource = -not [string]::IsNullOrWhiteSpace($request.QueryString["source"])
      hasHeaderSource = -not [string]::IsNullOrWhiteSpace($request.Headers["X-Takaro-Mod-Source"])
      atUtc = [DateTime]::UtcNow.ToString("o")
    }) | Out-Null

    if (-not (Test-Source $request)) {
      Add-Failure "$($request.HttpMethod) $path missing explicit TakaroConan source attribution"
      Write-JsonResponse $context 400 ([ordered]@{ ok = $false; error = "missing TakaroConan source" })
      continue
    }

    if ($request.HttpMethod -eq "GET" -and $path -eq "/mod/poll") {
      if ($commands.Count -gt 0) {
        Write-JsonResponse $context 200 ([ordered]@{ hasCommand = $true; command = $commands.Dequeue() })
      } else {
        Write-JsonResponse $context 200 ([ordered]@{ hasCommand = $false })
      }
      continue
    }

    if ($request.HttpMethod -eq "POST" -and $path -eq "/mod/result") {
      $body = Read-JsonBody $request
      if ($null -eq $body.requestId) {
        Add-Failure "/mod/result missing requestId"
      }
      if ($null -eq $body.result) {
        Add-Failure "/mod/result missing nested result object"
      } elseif ($null -eq $body.result.success) {
        Add-Failure "/mod/result nested result missing success"
      }
      if ($null -ne $body.success) {
        Add-Failure "/mod/result used top-level success; required shape is body.result.success"
      }
      $results.Add($body) | Out-Null
      Write-JsonResponse $context 200 ([ordered]@{ ok = $true })
      continue
    }

    if ($request.HttpMethod -eq "POST" -and $path -eq "/mod/event") {
      $body = Read-JsonBody $request
      if ($body.type -ne "chat-message") {
        Add-Failure "/mod/event type is not chat-message"
      }
      if ($body.data.message -ne "$marker inbound") {
        Add-Failure "/mod/event inbound marker mismatch; expected '$marker inbound'"
      }
      if (-not (Test-SteamIdentity $body.data.player)) {
        Add-Failure "/mod/event player is missing stable Steam/platform identity"
      }
      $events.Add($body) | Out-Null
      Write-JsonResponse $context 200 ([ordered]@{ ok = $true })
      continue
    }

    Write-JsonResponse $context 404 ([ordered]@{ ok = $false; error = "unknown route" })
  }
} finally {
  if ($listener.IsListening) {
    $listener.Stop()
  }
}

$serverWideOk = @($results | Where-Object { $_.requestId -eq "$marker-server-wide" -and $_.result.success -eq $true }).Count -gt 0
$targetedOk = @($results | Where-Object { $_.requestId -eq "$marker-targeted" -and $_.result.success -eq $true }).Count -gt 0
$inboundOk = @($events | Where-Object { $_.type -eq "chat-message" -and $_.data.message -eq "$marker inbound" -and (Test-SteamIdentity $_.data.player) }).Count -gt 0

if (-not $serverWideOk) {
  Add-Failure "did not receive successful nested /mod/result for server-wide command"
}
if (-not $targetedOk) {
  Add-Failure "did not receive successful nested /mod/result for targeted command"
}
if (-not $inboundOk) {
  Add-Failure "did not receive inbound chat /mod/event with marker and stable identity"
}

if ($failures.Count -eq 0) {
  Save-Receipt $OutputPath "passed"
  Write-Host "TakaroConan DevKit bridge contract smoke passed. Receipt: $OutputPath"
  exit 0
}

Save-Receipt $OutputPath "failed"
Write-Host "TakaroConan DevKit bridge contract smoke failed. Receipt: $OutputPath" -ForegroundColor Red
exit 1
