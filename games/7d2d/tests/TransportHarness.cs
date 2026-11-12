using System;
using System.Collections.Generic;
using System.Net;
using System.Net.Sockets;
using System.Threading;
using Takaro.WebSocket;

// Runs StalledSocket against the websocket-sharp build the mod ships, under
// Mono, with real local sockets: a peer that accepts TCP and then says nothing
// keeps Connect() blocked, Close() cannot stop it, and shutting down the TCP
// connection does.
public static class TransportHarness
{
    private static int _failures;

    public static int Main()
    {
        True(
            StalledSocket.CanAbort,
            "the shipped websocket-sharp keeps its TcpClient where StalledSocket looks"
        );

        SilentPeerBlocksConnectUntilAborted("wss");
        SilentPeerBlocksConnectUntilAborted("ws");
        RefusedConnectFailsOnItsOwn();
        AbortBeforeAnyConnectionIsANoOp();

        if (_failures > 0)
        {
            Console.Error.WriteLine($"transport harness: {_failures} failure(s)");
            return 1;
        }
        Console.WriteLine("transport harness: all assertions passed");
        return 0;
    }

    private static void SilentPeerBlocksConnectUntilAborted(string scheme)
    {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        int port = ((IPEndPoint)listener.LocalEndpoint).Port;
        var accepted = new List<TcpClient>();
        var acceptor = new Thread(() =>
        {
            try
            {
                while (true)
                    accepted.Add(listener.AcceptTcpClient());
            }
            catch (Exception)
            {
                // Listener stopped.
            }
        })
        {
            IsBackground = true,
        };
        acceptor.Start();

        var socket = new WebSocketSharp.WebSocket($"{scheme}://127.0.0.1:{port}/");
        socket.Log.Output = (data, path) => { };
        var closed = new ManualResetEvent(false);
        ushort closeCode = 0;
        socket.OnClose += (sender, e) =>
        {
            closeCode = e.Code;
            closed.Set();
        };

        var connectReturned = new ManualResetEvent(false);
        var connector = new Thread(() =>
        {
            try
            {
                socket.Connect();
            }
            catch (Exception)
            {
                // Reported through OnClose as well.
            }
            connectReturned.Set();
        })
        {
            IsBackground = true,
        };
        connector.Start();

        True(
            !connectReturned.WaitOne(3000),
            $"{scheme}: Connect() is still blocked 3 s after a silent peer accepted TCP"
        );
        Equal(
            WebSocketSharp.WebSocketState.Connecting,
            socket.ReadyState,
            $"{scheme}: the socket stays Connecting"
        );
        True(StalledSocket.HasTcpConnection(socket), $"{scheme}: the TCP connection is visible");

        // websocket-sharp's own Close() waits for the connect: it is no way out.
        var closeReturned = new ManualResetEvent(false);
        var closer = new Thread(() =>
        {
            try
            {
                socket.Close();
            }
            catch (Exception)
            {
                // Irrelevant here.
            }
            closeReturned.Set();
        })
        {
            IsBackground = true,
        };
        closer.Start();
        True(
            !closeReturned.WaitOne(1500),
            $"{scheme}: Close() does not return while the connect is stuck"
        );

        var watch = System.Diagnostics.Stopwatch.StartNew();
        True(
            StalledSocket.AbortTransport(socket),
            $"{scheme}: the stalled TCP connection is shut down"
        );
        True(
            connectReturned.WaitOne(3000),
            $"{scheme}: Connect() returns once the transport is shut down"
        );
        True(
            watch.ElapsedMilliseconds < 3000,
            $"{scheme}: within 3 s ({watch.ElapsedMilliseconds} ms)"
        );
        True(closed.WaitOne(3000), $"{scheme}: the failed connect is reported through OnClose");
        True(
            closeCode == 1006 || closeCode == 1015,
            $"{scheme}: as an abnormal close (code {closeCode})"
        );
        True(closeReturned.WaitOne(3000), $"{scheme}: a waiting Close() returns too");
        Equal(
            WebSocketSharp.WebSocketState.Closed,
            socket.ReadyState,
            $"{scheme}: the socket ends Closed"
        );

        listener.Stop();
        foreach (TcpClient client in accepted)
            client.Close();
    }

    private static void RefusedConnectFailsOnItsOwn()
    {
        var probe = new TcpListener(IPAddress.Loopback, 0);
        probe.Start();
        int port = ((IPEndPoint)probe.LocalEndpoint).Port;
        probe.Stop();

        var socket = new WebSocketSharp.WebSocket($"wss://127.0.0.1:{port}/");
        socket.Log.Output = (data, path) => { };
        var closed = new ManualResetEvent(false);
        socket.OnClose += (sender, e) => closed.Set();
        var watch = System.Diagnostics.Stopwatch.StartNew();
        socket.Connect();
        True(watch.ElapsedMilliseconds < 3000, "a refused connect returns at once");
        True(closed.WaitOne(1000), "a refused connect is reported through OnClose");
        True(
            !StalledSocket.AbortTransport(socket),
            "nothing is left to shut down after a refused connect"
        );
    }

    private static void AbortBeforeAnyConnectionIsANoOp()
    {
        var socket = new WebSocketSharp.WebSocket("wss://127.0.0.1:1/");
        True(!StalledSocket.HasTcpConnection(socket), "a new socket has no TCP connection");
        True(
            !StalledSocket.AbortTransport(socket),
            "aborting a socket that never connected does nothing"
        );
        True(!StalledSocket.AbortTransport(null), "aborting null does nothing");
    }

    private static void True(bool condition, string message)
    {
        if (condition)
        {
            Console.WriteLine("ok   " + message);
            return;
        }
        _failures++;
        Console.Error.WriteLine("FAIL " + message);
    }

    private static void Equal<T>(T expected, T actual, string message)
    {
        True(
            EqualityComparer<T>.Default.Equals(expected, actual),
            $"{message} (expected {expected}, got {actual})"
        );
    }
}
