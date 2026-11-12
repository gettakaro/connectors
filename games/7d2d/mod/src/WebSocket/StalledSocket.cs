using System;
using System.Net.Sockets;
using System.Reflection;

namespace Takaro.WebSocket
{
    /// <summary>
    /// Gives up on a websocket-sharp client socket that is stuck while
    /// connecting. websocket-sharp's Connect() has no timeout for the TLS
    /// handshake or the HTTP upgrade, and Close() or Dispose() during a connect
    /// wait for the same state lock the connect holds, so neither can stop it.
    /// Shutting down the TCP connection underneath is what wakes the blocked
    /// read: the handshake then fails and the library reports the close.
    ///
    /// websocket-sharp keeps the connection in the private field _tcpClient
    /// (sta/websocket-sharp at the commit the catalog pins). If a different
    /// build lacks it, CanAbort is false and a stalled attempt is only
    /// detached, never closed.
    /// </summary>
    public static class StalledSocket
    {
        private static readonly FieldInfo TcpClientField =
            typeof(WebSocketSharp.WebSocket).GetField(
                "_tcpClient",
                BindingFlags.NonPublic | BindingFlags.Instance
            );

        public static bool CanAbort => TcpClientField != null;

        /// <summary>True once the TCP connection under the socket exists.</summary>
        public static bool HasTcpConnection(WebSocketSharp.WebSocket socket)
        {
            return TcpClientOf(socket) != null;
        }

        /// <summary>
        /// Shuts down and closes the TCP connection under the socket. Returns
        /// false when there is none yet (still resolving or in the TCP connect,
        /// which the operating system times out on its own).
        /// </summary>
        public static bool AbortTransport(WebSocketSharp.WebSocket socket)
        {
            TcpClient client = TcpClientOf(socket);
            if (client == null)
                return false;

            try
            {
                // shutdown(2) wakes a read blocked on another thread on Linux;
                // close(2) alone does not.
                client.Client?.Shutdown(SocketShutdown.Both);
            }
            catch (Exception)
            {
                // Already closed or never connected; Close() below still runs.
            }

            try
            {
                client.Close();
            }
            catch (Exception)
            {
                // Nothing left to release.
            }
            return true;
        }

        private static TcpClient TcpClientOf(WebSocketSharp.WebSocket socket)
        {
            if (socket == null || TcpClientField == null)
                return null;
            try
            {
                return TcpClientField.GetValue(socket) as TcpClient;
            }
            catch (Exception)
            {
                return null;
            }
        }
    }
}
