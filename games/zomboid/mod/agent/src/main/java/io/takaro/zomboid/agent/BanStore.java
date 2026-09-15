package io.takaro.zomboid.agent;

import com.google.gson.Gson;
import com.google.gson.GsonBuilder;
import com.google.gson.reflect.TypeToken;

import java.lang.reflect.Type;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.time.Instant;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Persistent record of Takaro-issued bans, including timed-ban expiry times that
 * Project Zomboid's own ban tables cannot express.
 *
 * <p>Stored as JSON at {@code /home/steam/Zomboid/Takaro/bans.json}. Accessed on
 * the main thread (ban/unban actions and the reconciler's expiry sweep), but
 * guarded with {@code synchronized} so nothing can corrupt it.
 */
public final class BanStore {

    /** One banned player. {@code expiresAt} is an ISO-8601 instant, or null for a permanent ban. */
    public static final class Ban {
        public String gameId;    // username
        public String steamId;   // may be null for an offline ban
        public String name;
        public String reason;
        public String expiresAt; // ISO-8601, or null

        Ban() {
        }

        Ban(String gameId, String steamId, String name, String reason, String expiresAt) {
            this.gameId = gameId;
            this.steamId = steamId;
            this.name = name;
            this.reason = reason;
            this.expiresAt = expiresAt;
        }
    }

    private static final Gson GSON = new GsonBuilder().setPrettyPrinting().create();
    private static final Type LIST_TYPE = new TypeToken<List<Ban>>() {}.getType();

    private final Path file;
    private final Map<String, Ban> bans = new LinkedHashMap<>();

    public BanStore() {
        this(Paths.get(System.getProperty("takaro.bansFile",
                "/home/steam/Zomboid/Takaro/bans.json")));
    }

    public BanStore(Path file) {
        this.file = file;
        load();
    }

    private synchronized void load() {
        try {
            if (Files.isReadable(file)) {
                String json = Files.readString(file, StandardCharsets.UTF_8);
                List<Ban> loaded = GSON.fromJson(json, LIST_TYPE);
                if (loaded != null) {
                    for (Ban b : loaded) {
                        if (b != null && b.gameId != null) {
                            bans.put(b.gameId, b);
                        }
                    }
                }
                AgentLog.log("banstore: loaded " + bans.size() + " ban(s) from " + file);
            }
        } catch (Exception e) {
            AgentLog.error("banstore: failed to load " + file, e);
        }
    }

    private synchronized void save() {
        try {
            Path parent = file.getParent();
            if (parent != null) {
                Files.createDirectories(parent);
            }
            Files.writeString(file, GSON.toJson(new ArrayList<>(bans.values()), LIST_TYPE),
                    StandardCharsets.UTF_8);
        } catch (Exception e) {
            AgentLog.error("banstore: failed to save " + file, e);
        }
    }

    public synchronized void put(String gameId, String steamId, String name, String reason, String expiresAt) {
        bans.put(gameId, new Ban(gameId, steamId, name, reason, expiresAt));
        save();
    }

    public synchronized Ban remove(String gameId) {
        Ban removed = bans.remove(gameId);
        if (removed != null) {
            save();
        }
        return removed;
    }

    public synchronized Ban get(String gameId) {
        return bans.get(gameId);
    }

    public synchronized List<Ban> list() {
        return new ArrayList<>(bans.values());
    }

    /** Bans whose {@code expiresAt} is at or before {@code now}. */
    public synchronized List<Ban> expired(Instant now) {
        List<Ban> out = new ArrayList<>();
        for (Ban b : bans.values()) {
            if (b.expiresAt == null || b.expiresAt.isEmpty()) {
                continue;
            }
            try {
                if (!Instant.parse(b.expiresAt).isAfter(now)) {
                    out.add(b);
                }
            } catch (Exception e) {
                // unparseable expiry — leave it as a permanent ban
            }
        }
        return out;
    }
}
