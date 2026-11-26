package dev.takaro.hytale.state;

import com.google.gson.Gson;
import com.google.gson.GsonBuilder;
import com.google.gson.reflect.TypeToken;

import java.io.IOException;
import java.io.Reader;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Collection;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;

/**
 * A small persisted ledger of every player the server has seen.
 *
 * <p>Hytale's own state only knows about players that are currently connected, and a
 * {@code Ban} entry carries a UUID but no name. Takaro, however, asks for an
 * {@code IGamePlayer} (gameId + name + platformId) for offline players - for
 * {@code getPlayer}, and for every row of {@code listBans}. This ledger is what makes those
 * answers real instead of empty.
 *
 * <p>Only what the server already published to Takaro is stored: game id, name, platform id
 * and the IP last seen. It is written next to the connector's own config file.
 *
 * <p>No Hytale imports, so it can be unit-tested without the server jar.
 */
public class KnownPlayers {
    /** One remembered player. */
    public static final class Entry {
        public String gameId;
        public String name;
        public String platformId;
        public String ip;
        public long lastSeen;

        public Entry() {
        }

        public Entry(String gameId, String name, String platformId, String ip, long lastSeen) {
            this.gameId = gameId;
            this.name = name;
            this.platformId = platformId;
            this.ip = ip;
            this.lastSeen = lastSeen;
        }
    }

    private static final Gson GSON = new GsonBuilder().setPrettyPrinting().create();

    private final Path file;
    private final Map<String, Entry> byId = new ConcurrentHashMap<>();
    private final Object saveLock = new Object();

    public KnownPlayers(Path file) {
        this.file = file;
        load();
    }

    private void load() {
        if (file == null || !Files.isRegularFile(file)) {
            return;
        }
        try (Reader reader = Files.newBufferedReader(file, StandardCharsets.UTF_8)) {
            Map<String, Entry> loaded = GSON.fromJson(reader,
                new TypeToken<Map<String, Entry>>() { }.getType());
            if (loaded != null) {
                loaded.forEach((k, v) -> {
                    if (k != null && v != null) {
                        byId.put(k, v);
                    }
                });
            }
        } catch (Exception ignored) {
            // A corrupt ledger must never stop the connector from starting.
        }
    }

    private void save() {
        if (file == null) {
            return;
        }
        synchronized (saveLock) {
            try {
                if (file.getParent() != null) {
                    Files.createDirectories(file.getParent());
                }
                Path tmp = file.resolveSibling(file.getFileName() + ".tmp");
                try (Writer writer = Files.newBufferedWriter(tmp, StandardCharsets.UTF_8)) {
                    GSON.toJson(byId, writer);
                }
                Files.move(tmp, file, java.nio.file.StandardCopyOption.REPLACE_EXISTING);
            } catch (IOException ignored) {
                // Losing the ledger is not worth failing a player connect over.
            }
        }
    }

    /** Remember (or refresh) a player. Returns the stored entry. */
    public Entry record(String gameId, String name, String platformId, String ip) {
        if (gameId == null || gameId.isEmpty()) {
            return null;
        }
        Entry existing = byId.get(gameId);
        Entry entry = existing == null ? new Entry() : existing;
        entry.gameId = gameId;
        if (name != null && !name.isEmpty()) {
            entry.name = name;
        }
        entry.platformId = platformId != null ? platformId : "hytale:" + gameId;
        if (ip != null && !ip.isEmpty()) {
            entry.ip = ip;
        }
        entry.lastSeen = System.currentTimeMillis();
        byId.put(gameId, entry);
        save();
        return entry;
    }

    public Entry get(String gameId) {
        return gameId == null ? null : byId.get(gameId);
    }

    public Entry getByName(String name) {
        if (name == null) {
            return null;
        }
        String wanted = name.toLowerCase(Locale.ROOT);
        for (Entry e : byId.values()) {
            if (e.name != null && e.name.toLowerCase(Locale.ROOT).equals(wanted)) {
                return e;
            }
        }
        return null;
    }

    /** The name last seen for this game id, or null if the server has never seen it. */
    public String nameOf(String gameId) {
        Entry e = get(gameId);
        return e == null ? null : e.name;
    }

    public Collection<Entry> all() {
        return new ArrayList<>(byId.values());
    }

    public int size() {
        return byId.size();
    }

    /** The IGamePlayer map Takaro expects, or null when the player is unknown. */
    public Map<String, Object> toGamePlayer(String gameId) {
        Entry e = get(gameId);
        if (e == null) {
            return null;
        }
        Map<String, Object> player = new java.util.HashMap<>();
        player.put("gameId", e.gameId);
        player.put("name", e.name);
        player.put("platformId", e.platformId != null ? e.platformId : "hytale:" + e.gameId);
        if (e.ip != null) {
            player.put("ip", e.ip);
        }
        return player;
    }

    /**
     * The minimal {@code IGamePlayer} for a game id this server has never seen.
     *
     * <p>Takaro rejects every "no player" answer to {@code getPlayer}: {@code {}} fails
     * {@code gameId isString}, and both {@code null} and an error frame produce
     * "No payload provided but expected DTO: IGamePlayer" - each one a user-visible HTTP 400
     * telling the operator their mod is out of date. A connector must therefore always answer
     * with a real player shape, so an unknown id is echoed back as its own name rather than
     * invented: the same convention {@code Bans.toIBan} already uses for a nameless ban.
     *
     * @return the synthesised record, or {@code null} if there is no id to build it from
     */
    public static Map<String, Object> synthesize(String gameId) {
        if (gameId == null || gameId.isEmpty()) {
            return null;
        }
        Map<String, Object> player = new java.util.HashMap<>();
        player.put("gameId", gameId);
        player.put("name", gameId);
        player.put("platformId", "hytale:" + gameId);
        player.put("online", false);
        return player;
    }

    /** All remembered game ids, for diagnostics. */
    public List<String> ids() {
        return new ArrayList<>(byId.keySet());
    }
}
