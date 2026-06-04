import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WEBSOCKET = (ROOT / '7d2d/src/WebSocket/WebSocketClient.cs').read_text()
API = (ROOT / '7d2d/src/API.cs').read_text()
CONFIG = (ROOT / '7d2d/src/Config/ConfigManager.cs').read_text()
README = (ROOT / '7d2d/README.md').read_text()


def method_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 0
    for index in range(brace, len(source)):
        char = source[index]
        if char == '{':
            depth += 1
        elif char == '}':
            depth -= 1
            if depth == 0:
                return source[brace:index + 1]
    raise AssertionError(f'Could not extract body for {signature}')


def compact(source: str) -> str:
    return ''.join(source.split())


class SevenD2DConnectorRegressionTests(unittest.TestCase):
    def test_list_bans_reads_blocked_players_and_admin_blacklist(self):
        body = method_body(WEBSOCKET, 'private void HandleListBans(string requestId)')

        self.assertIn('Platform.BlockedPlayerList.Instance.GetEntriesOrdered', body)
        self.assertIn('GameManager.Instance.adminTools.Blacklist.GetBanned()', body)
        self.assertNotIn('else\n                {\n                    // Fallback to AdminTools blacklist system', body)
        self.assertIn('seenBanIds', body)
        self.assertIn('string nativeId = blockedEntry.PlayerData.NativeId?.CombinedString', body)
        self.assertIn('seenBanIds.Contains(nativeId)', body)
        self.assertIn('seenBanIds.Add(nativeId)', body)
        self.assertIn('ban.BanReason', body)
        self.assertIn('GameManager.Instance?.adminTools?.Blacklist != null', body)
        self.assertIn('playerList?.GetPlayerData(ban.UserIdentifier)', compact(body))
        self.assertIn('ban.BannedUntil.ToString("o")', body)

    def test_reconnect_is_indefinite_with_backoff_cap(self):
        body = method_body(WEBSOCKET, 'private void ScheduleReconnect()')

        self.assertNotIn('MAX_RECONNECT_ATTEMPTS', WEBSOCKET)
        self.assertNotIn('Giving up', body)
        self.assertIn('_reconnectAttempts++', body)
        self.assertIn('Math.Pow(2', body)
        self.assertIn('MAX_RECONNECT_INTERVAL_SECONDS', body)

    def test_log_event_forwards_non_takaro_raw_log_lines(self):
        body = method_body(API, 'private void HandleLogMessage(string logString, string stackTrace, LogType type)')

        self.assertIn('SendLogEvent(logString)', body)
        self.assertNotIn('type == LogType.Error || type == LogType.Warning', body)
        self.assertNotIn('formattedMessage = $"[{type}] {logString}"', body)

    def test_default_websocket_url_points_to_production_connect_endpoint(self):
        self.assertIn('wss://connect.takaro.io/', CONFIG)
        self.assertNotIn('wss://your-takaro-websocket-server.com', CONFIG)
        self.assertIn('RegistrationToken', README)
        self.assertIn('wss://connect.takaro.io/', README)


if __name__ == '__main__':
    unittest.main()
