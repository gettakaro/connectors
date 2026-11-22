#if TAKARO_VALHEIM_PLUGIN
using System.Reflection;
using Takaro.Valheim.Core;

namespace Takaro.Valheim.Plugin;

/// <summary>
/// Reads a Steam peer's round-trip time on a dedicated server.
/// The game's own <c>ZSteamSocket.GetConnectionQuality</c> asks the Steam client API, which a dedicated
/// server never initialises, so it always reports 0. The game-server API holds the real value; it is
/// reached by reflection because the plugin does not reference the Steamworks assembly.
/// </summary>
internal static class SteamServerPing
{
    private static readonly Lazy<Reader?> reader = new(CreateReader);

    /// <summary>The peer's ping in milliseconds, or null when the socket is not a Steam socket or Steam has no reading.</summary>
    public static int? TryRead(ISocket socket)
    {
        if (socket is not ZSteamSocket steamSocket || reader.Value is not { } read)
        {
            return null;
        }

        return read.Read(steamSocket);
    }

    private static Reader? CreateReader()
    {
        try
        {
            var sockets = Type.GetType("Steamworks.SteamGameServerNetworkingSockets, com.rlabrecque.steamworks.net");
            var statusType = Type.GetType("Steamworks.SteamNetConnectionRealTimeStatus_t, com.rlabrecque.steamworks.net");
            var laneType = Type.GetType("Steamworks.SteamNetConnectionRealTimeLaneStatus_t, com.rlabrecque.steamworks.net");
            var connection = typeof(ZSteamSocket).GetField("m_con", BindingFlags.Instance | BindingFlags.NonPublic);
            var pingField = statusType?.GetField("m_nPing", BindingFlags.Instance | BindingFlags.Public);
            if (sockets is null || statusType is null || laneType is null || connection is null || pingField is null)
            {
                return null;
            }

            var method = sockets.GetMethod(
                "GetConnectionRealTimeStatus",
                BindingFlags.Static | BindingFlags.Public,
                null,
                [connection.FieldType, statusType.MakeByRefType(), typeof(int), laneType.MakeByRefType()],
                null);
            return method is null ? null : new Reader(method, connection, statusType, laneType, pingField);
        }
        catch
        {
            return null;
        }
    }

    private sealed class Reader(MethodInfo method, FieldInfo connection, Type statusType, Type laneType, FieldInfo pingField)
    {
        private const int ResultOk = 1;

        public int? Read(ZSteamSocket socket)
        {
            var arguments = new[]
            {
                connection.GetValue(socket),
                Activator.CreateInstance(statusType),
                0,
                Activator.CreateInstance(laneType)
            };
            var result = method.Invoke(null, arguments);
            return Convert.ToInt32(result) == ResultOk
                ? PlayerMapper.ToSteamPing((int)pingField.GetValue(arguments[1]))
                : null;
        }
    }
}
#endif
