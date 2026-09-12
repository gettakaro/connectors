package io.takaro.zomboid.agent;

import io.takaro.zomboid.core.model.BanEntry;
import io.takaro.zomboid.core.model.CommandResult;
import io.takaro.zomboid.core.model.GameEntity;
import io.takaro.zomboid.core.model.GameItem;
import io.takaro.zomboid.core.model.GameLocation;
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
import zombie.core.logger.LoggerManager;
import zombie.core.logger.ZLogger;
import zombie.core.raknet.UdpConnection;
import zombie.inventory.InventoryItem;
import zombie.inventory.ItemContainer;
import zombie.inventory.types.HandWeapon;
import zombie.iso.areas.SafeHouse;
import zombie.network.DBBannedSteamID;
import zombie.network.GameServer;
import zombie.network.ServerWorldDatabase;
import zombie.network.chat.ChatServer;
import zombie.network.server.EventManager;
import zombie.network.server.IEventController;
import zombie.scripting.ScriptManager;
import zombie.scripting.entity.GameEntityTemplate;
import zombie.scripting.objects.Item;
import zombie.scripting.objects.VehicleScript;

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

    // --- M2: shutdown ---

    /** Graceful server shutdown — mirrors the {@code QuitCommand} path via {@code GameServer.rcon}. */
    public static void shutdown() {
        GameServer.rcon("quit");
    }

    // --- M2: item catalogue ---

    /** Raw rows from {@code ScriptManager.instance.getAllItems()} for {@link Catalog#buildItems}. */
    @SuppressWarnings("unchecked")
    public static List<Catalog.ItemRow> itemRows() {
        List<Catalog.ItemRow> rows = new ArrayList<>();
        ScriptManager sm = ScriptManager.instance;
        if (sm == null) {
            return rows;
        }
        ArrayList<Item> items = sm.getAllItems();
        if (items == null) {
            return rows;
        }
        for (Item it : items) {
            if (it == null) {
                continue;
            }
            rows.add(new Catalog.ItemRow(it.getFullName(), it.getDisplayName(), it.getDisplayCategory()));
        }
        return rows;
    }

    public static List<GameItem> listItems() {
        return Catalog.buildItems(itemRows());
    }

    // --- M2: player inventory ---

    @SuppressWarnings("unchecked")
    public static List<io.takaro.zomboid.core.model.InventoryItem> getPlayerInventory(String username) {
        IsoPlayer p = findPlayer(username);
        if (p == null) {
            return null;
        }
        ItemContainer inv = p.getInventory();
        List<Catalog.InvRow> rows = new ArrayList<>();
        if (inv != null) {
            ArrayList<InventoryItem> items = inv.getItems();
            if (items != null) {
                for (InventoryItem it : items) {
                    if (it == null) {
                        continue;
                    }
                    rows.add(new Catalog.InvRow(it.getFullType(), it.getDisplayName(),
                            it.getCount(), it.getCondition(), it.getConditionMax()));
                }
            }
        }
        return Catalog.groupInventory(rows);
    }

    // --- M2: entities (static Zombie + entity templates + vehicle scripts) ---

    public static List<GameEntity> listEntities() {
        List<GameEntity> out = new ArrayList<>();
        out.add(new GameEntity("Zombie", "Zombie", "The Project Zomboid infected", "hostile"));
        ScriptManager sm = ScriptManager.instance;
        if (sm == null) {
            return out;
        }
        ArrayList<?> templates = sm.getAllGameEntityTemplates();
        if (templates != null) {
            for (Object obj : templates) {
                if (obj instanceof GameEntityTemplate g) {
                    String code = g.name; // GameEntityTemplate exposes a public `name` field
                    if (code == null || code.isEmpty()) {
                        continue;
                    }
                    out.add(new GameEntity(code, code, "Game entity template", "entity"));
                }
            }
        }
        ArrayList<?> vehicles = sm.getAllVehicleScripts();
        if (vehicles != null) {
            for (Object obj : vehicles) {
                if (obj instanceof VehicleScript v) {
                    String code = v.getFullName();
                    if (code == null || code.isEmpty()) {
                        code = v.getName();
                    }
                    if (code == null || code.isEmpty()) {
                        continue;
                    }
                    out.add(new GameEntity(code, v.getName() != null ? v.getName() : code,
                            "Vehicle", "vehicle"));
                }
            }
        }
        return out;
    }

    // --- M2: locations (safehouses) ---

    /**
     * Safehouses, read from the package-private static {@code SafeHouse.safehouseList}
     * (PZ exposes no public full-list accessor — only per-owner / per-square
     * lookups). An empty world has no safehouses, so this is normally {@code []}.
     */
    @SuppressWarnings("unchecked")
    public static List<GameLocation> listLocations() {
        List<GameLocation> out = new ArrayList<>();
        try {
            Field f = SafeHouse.class.getDeclaredField("safehouseList");
            f.setAccessible(true);
            Object raw = f.get(null);
            if (raw instanceof ArrayList<?> list) {
                for (Object o : list) {
                    if (o instanceof SafeHouse sh) {
                        int x = sh.getX();
                        int y = sh.getY();
                        int w = sh.getW();
                        int h = sh.getH();
                        String title = sh.getTitle();
                        String owner = sh.getOwner();
                        String name = title != null && !title.isEmpty() ? title
                                : (owner != null && !owner.isEmpty() ? owner + "'s safehouse" : "Safehouse");
                        // centre of the claimed rectangle
                        double cx = x + w / 2.0;
                        double cy = y + h / 2.0;
                        out.add(new GameLocation(name, name, cx, cy, 0.0, null, null,
                                (double) w, (double) h, 0.0));
                    }
                }
            }
        } catch (Throwable t) {
            AgentLog.log("listLocations: safehouse list unavailable (" + t.getClass().getSimpleName() + ")");
        }
        return out;
    }

    // --- M2: entity-killed weapon decode ---

    /** {@code weapon.getFullType()} for the entity-killed event; "" when no weapon. */
    public static String weaponFullType(Object weapon) {
        if (weapon instanceof HandWeapon hw) {
            String t = hw.getFullType();
            return t != null ? t : "";
        }
        return "";
    }

    // --- M2: log event plumbing ---

    /** Register a text-only server report callback on the EventManager (for the {@code log} event). */
    public static boolean registerLogCallback(Object controller) {
        EventManager em = EventManager.instance();
        if (em == null) {
            return false;
        }
        em.registerCallback((IEventController) controller);
        return true;
    }

    /** Lazily-resolved {@code user} logger, cached, for filtering {@code ZLogger.write}. */
    private static volatile ZLogger userLogger;
    private static volatile boolean userLoggerResolved;

    public static boolean isUserLogger(Object zlogger) {
        if (!(zlogger instanceof ZLogger)) {
            return false;
        }
        ZLogger target = userLogger;
        if (!userLoggerResolved) {
            try {
                target = LoggerManager.getLogger("user");
            } catch (Throwable t) {
                target = null;
            }
            userLogger = target;
            userLoggerResolved = true;
        }
        return target != null && zlogger == target;
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
