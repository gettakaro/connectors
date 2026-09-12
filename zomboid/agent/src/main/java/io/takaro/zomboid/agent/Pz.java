package io.takaro.zomboid.agent;

import io.takaro.zomboid.core.model.BanEntry;
import io.takaro.zomboid.core.model.CommandResult;
import io.takaro.zomboid.core.model.PlayerInfo;
import io.takaro.zomboid.core.model.PlayerLocation;

import java.lang.reflect.Field;
import java.sql.Connection;
import java.sql.ResultSet;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.List;

import zombie.characters.IsoPlayer;
import zombie.chat.ChatMessage;
import zombie.core.raknet.UdpConnection;
import zombie.inventory.ItemContainer;
import zombie.network.DBBannedSteamID;
import zombie.network.GameServer;
import zombie.network.ServerWorldDatabase;
import zombie.network.chat.ChatServer;
import zombie.scripting.ScriptManager;
import zombie.scripting.objects.Item;

/**
 * Typed facade over the Project Zomboid B42 (42.20.4) server classes.
 *
 * <p>Every method here links directly against the game classes (compiled
 * {@code compileOnly} against {@code projectzomboid.jar}); the only reflection
 * is {@link ServerWorldDatabase}'s package-private {@code conn} field, read to
 * list banned usernames for {@code listBans} completeness.
 *
 * <p>All methods that touch {@link GameServer} state MUST be called on the game
 * main thread. The call sequences for {@link #teleport} and {@link #giveItem}
 * mirror {@code TeleportToCommand.TeleportUserToCoords()} and
 * {@code AddItemCommand.Command()} respectively, pinned from the 42.20.4 jar.
 */
public final class Pz {

    private Pz() {
    }

    // --- lifecycle / reachability ---

    public static boolean reachable() {
        return GameServer.udpEngine != null && !GameServer.closed;
    }

    public static Thread mainThread() {
        return GameServer.mainThread;
    }

    // --- player queries ---

    @SuppressWarnings("unchecked")
    public static List<PlayerInfo> getPlayers() {
        List<PlayerInfo> out = new ArrayList<>();
        ArrayList<IsoPlayer> players = GameServer.getPlayers();
        if (players == null) {
            return out;
        }
        for (IsoPlayer p : players) {
            if (p != null) {
                out.add(toPlayerInfo(p));
            }
        }
        return out;
    }

    public static PlayerInfo getPlayer(String username) {
        IsoPlayer p = findPlayer(username);
        return p != null ? toPlayerInfo(p) : null;
    }

    public static PlayerLocation getPlayerLocation(String username) {
        IsoPlayer p = findPlayer(username);
        if (p == null) {
            return null;
        }
        return new PlayerLocation(p.getX(), p.getY(), p.getZ(), null);
    }

    public static PlayerInfo toPlayerInfo(IsoPlayer p) {
        String username = p.getUsername();
        String name = p.getDisplayName();
        if (name == null || name.isEmpty()) {
            name = username;
        }
        long steam = p.getSteamID();
        String steamId = steam != 0L ? Long.toString(steam) : null;
        String platformId = steamId != null ? PlayerInfo.buildPlatformId(steamId) : null;

        String ip = null;
        int ping = p.getPing();
        UdpConnection conn = GameServer.getConnectionFromPlayer(p);
        if (conn != null) {
            ip = conn.getIP();
            ping = conn.getAveragePing();
        }
        // gameId is the username in PZ; platform is steam.
        return new PlayerInfo(username, name, steamId, null, null, platformId, ip, ping);
    }

    /** Resolve by username, falling back to a steamId scan for callers that pass a steam id. */
    public static IsoPlayer findPlayer(String gameId) {
        if (gameId == null) {
            return null;
        }
        IsoPlayer p = GameServer.getPlayerByUserName(gameId);
        if (p != null) {
            return p;
        }
        // fallback: gameId might be a steamId
        long wanted;
        try {
            wanted = Long.parseLong(gameId);
        } catch (NumberFormatException e) {
            return null;
        }
        ArrayList<IsoPlayer> players = GameServer.getPlayers();
        if (players != null) {
            for (IsoPlayer candidate : players) {
                if (candidate != null && candidate.getSteamID() == wanted) {
                    return candidate;
                }
            }
        }
        return null;
    }

    // --- messaging ---

    public static void sendMessageGlobal(String msg) {
        ChatServer cs = ChatServer.getInstance();
        if (cs != null) {
            cs.sendMessageToServerChat(msg);
        }
    }

    /** Targeted chat to a single player; returns false if the player/connection is gone. */
    public static boolean sendMessageTo(String username, String msg) {
        IsoPlayer p = findPlayer(username);
        if (p == null) {
            return false;
        }
        UdpConnection conn = GameServer.getConnectionFromPlayer(p);
        if (conn == null) {
            return false;
        }
        ChatServer cs = ChatServer.getInstance();
        if (cs == null) {
            return false;
        }
        cs.sendMessageToServerChat(conn, msg);
        return true;
    }

    // --- console ---

    public static CommandResult rcon(String command) {
        String raw = GameServer.rcon(command);
        if (raw == null) {
            raw = "";
        }
        boolean success = !raw.startsWith("Unknown command");
        return new CommandResult(success, raw, success ? null : raw);
    }

    // --- moderation ---

    /** kick + force-disconnect; mirrors KickUserCommand's effect. */
    public static boolean kick(String username, String reason) {
        IsoPlayer p = findPlayer(username);
        if (p == null) {
            return false;
        }
        UdpConnection conn = GameServer.getConnectionFromPlayer(p);
        if (conn == null) {
            return false;
        }
        String r = reason == null || reason.isEmpty() ? "Kicked by Takaro" : reason;
        GameServer.kick(conn, "You have been kicked from this server.", r);
        conn.forceDisconnect("kick");
        return true;
    }

    /**
     * Mirror of {@code TeleportToCommand.TeleportUserToCoords()}: resolve the
     * player by username, then {@code GameServer.sendTeleport(player, x, y, z)}
     * with raw float coords (the command only floors them for its log line).
     */
    public static boolean teleport(String username, double x, double y, double z) {
        IsoPlayer p = findPlayer(username);
        if (p == null) {
            return false;
        }
        GameServer.sendTeleport(p, (float) x, (float) y, (float) z);
        return true;
    }

    /**
     * Mirror of {@code AddItemCommand.Command()}: validate the item via
     * {@code ScriptManager.instance.FindItem(code)}, add it to the player's
     * inventory with {@code ItemContainer.AddItems(type, count)}, then sync to
     * the client with {@code GameServer.sendAddItemsToContainer}.
     */
    public static String giveItem(String username, String code, int amount) {
        Item item = ScriptManager.instance.FindItem(code);
        if (item == null) {
            return "No such item: " + code;
        }
        IsoPlayer p = findPlayer(username);
        if (p == null) {
            return "No such user: " + username;
        }
        ItemContainer inv = p.getInventory();
        if (inv == null) {
            return "Player has no inventory";
        }
        int count = Math.max(1, amount);
        ArrayList<?> added = inv.AddItems(item.getFullName(), count);
        GameServer.sendAddItemsToContainer(inv, (ArrayList) added);
        return null;
    }

    // --- bans (PZ has no native ban expiry; timed bans live in BanStore) ---

    /** Returns the online player's steamId (0 if offline/unknown) so BanStore can record it. */
    public static long banUser(String username, String reason) {
        long steamId = 0L;
        IsoPlayer p = findPlayer(username);
        if (p != null) {
            steamId = p.getSteamID();
        }
        ServerWorldDatabase db = ServerWorldDatabase.instance;
        if (db != null) {
            try {
                db.banUser(username, true);
                if (steamId != 0L) {
                    db.banSteamID(Long.toString(steamId), reason == null ? "" : reason, true);
                }
            } catch (Exception e) {
                throw new IllegalStateException("ban DB write failed: " + e.getMessage(), e);
            }
        }
        return steamId;
    }

    public static void unbanUser(String username, String steamId) {
        ServerWorldDatabase db = ServerWorldDatabase.instance;
        if (db == null) {
            return;
        }
        try {
            db.banUser(username, false);
            if (steamId != null && !steamId.isEmpty() && !"0".equals(steamId)) {
                db.banSteamID(steamId, "", false);
            }
        } catch (Exception e) {
            throw new IllegalStateException("unban DB write failed: " + e.getMessage(), e);
        }
    }

    /** Banned steamId rows straight from the DB (bannedid table). */
    @SuppressWarnings("unchecked")
    public static List<BanEntry> bannedSteamIdRows() {
        List<BanEntry> out = new ArrayList<>();
        ServerWorldDatabase db = ServerWorldDatabase.instance;
        if (db == null) {
            return out;
        }
        try {
            ArrayList<DBBannedSteamID> rows = db.getBannedSteamIDs();
            if (rows != null) {
                for (DBBannedSteamID row : rows) {
                    if (row != null) {
                        out.add(new BanEntry(row.getSteamID(), row.getSteamID(), row.getReason(), null));
                    }
                }
            }
        } catch (Throwable t) {
            AgentLog.error("bannedSteamIdRows failed", t);
        }
        return out;
    }

    /**
     * Banned usernames from the {@code whitelist} table, read through the
     * package-private {@code ServerWorldDatabase.conn} field. Best-effort: PZ has
     * no dedicated banned column, so we look for the banned-role marker and fall
     * back to an empty list if the schema differs.
     */
    public static List<String> bannedUsernamesViaConn() {
        List<String> out = new ArrayList<>();
        ServerWorldDatabase db = ServerWorldDatabase.instance;
        if (db == null) {
            return out;
        }
        try {
            Field f = ServerWorldDatabase.class.getDeclaredField("conn");
            f.setAccessible(true);
            Connection conn = (Connection) f.get(db);
            if (conn == null) {
                return out;
            }
            try (Statement st = conn.createStatement();
                 ResultSet rs = st.executeQuery(
                         "SELECT username FROM whitelist WHERE banned = 1")) {
                while (rs.next()) {
                    out.add(rs.getString(1));
                }
            }
        } catch (Throwable t) {
            // schema without a `banned` column (the common case) or no DB yet —
            // BanStore remains the authoritative record of Takaro-issued bans.
            AgentLog.log("bannedUsernamesViaConn: unavailable (" + t.getClass().getSimpleName() + ")");
        }
        return out;
    }

    // --- chat message decode (safe to read off the main thread) ---

    public static boolean chatIsServerAuthor(Object chatMessage) {
        return ((ChatMessage) chatMessage).isServerAuthor();
    }

    public static boolean chatIsFromDiscord(Object chatMessage) {
        try {
            return ((ChatMessage) chatMessage).isFromDiscord();
        } catch (Throwable t) {
            return false;
        }
    }

    public static String chatAuthor(Object chatMessage) {
        return ((ChatMessage) chatMessage).getAuthor();
    }

    public static String chatText(Object chatMessage) {
        return ((ChatMessage) chatMessage).getText();
    }

    public static String chatChannel(Object chatMessage) {
        try {
            ChatMessage m = (ChatMessage) chatMessage;
            if (m.getChat() != null && m.getChat().getType() != null) {
                return m.getChat().getType().name();
            }
        } catch (Throwable t) {
            // fall through
        }
        return "general";
    }

    // --- death decode ---

    public static boolean isIsoPlayer(Object o) {
        return o instanceof IsoPlayer;
    }

    public static String playerUsername(Object isoPlayer) {
        return ((IsoPlayer) isoPlayer).getUsername();
    }

    public static String playerDisplayName(Object isoPlayer) {
        IsoPlayer p = (IsoPlayer) isoPlayer;
        String name = p.getDisplayName();
        return name == null || name.isEmpty() ? p.getUsername() : name;
    }

    public static double[] playerPos(Object isoPlayer) {
        IsoPlayer p = (IsoPlayer) isoPlayer;
        return new double[] {p.getX(), p.getY(), p.getZ()};
    }
}
