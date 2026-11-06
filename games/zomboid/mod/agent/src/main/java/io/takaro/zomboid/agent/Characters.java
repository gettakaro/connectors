package io.takaro.zomboid.agent;

import com.google.gson.Gson;
import com.google.gson.GsonBuilder;
import com.google.gson.reflect.TypeToken;
import io.takaro.zomboid.core.model.GameCharacter;

import java.io.IOException;
import java.lang.ref.WeakReference;
import java.lang.reflect.Type;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.UUID;
import java.util.function.Supplier;

/**
 * The id of each account's current survivor. Project Zomboid has none to offer: the row in
 * {@code players.db} is reused by the next survivor on the account, and the survivor's name is
 * not unique. So the connector issues one the first time it sees a living survivor, and issues
 * the next after a death, once the new survivor is alive.
 *
 * <p>The file lives in the world's save folder, so wiping the world forgets the ids with it.
 * Written on the main thread; read from the WebSocket thread, hence {@code synchronized}.
 */
public final class Characters {

    public static final String FILE_NAME = "takaro-characters.json";

    private static final class Entry {
        String id;
        String name;
        boolean ended;
        transient WeakReference<Object> deadBody;
    }

    private static final Gson GSON = new GsonBuilder().setPrettyPrinting().create();
    private static final Type MAP_TYPE = new TypeToken<Map<String, Entry>>() {}.getType();

    private final Supplier<Path> fileSource;
    private final Map<String, Entry> entries = new LinkedHashMap<>();
    private Path file;

    /** {@code fileSource} is called on first use: the save folder is unknown until the world loads. */
    public Characters(Supplier<Path> fileSource) {
        this.fileSource = fileSource;
    }

    /** Whether {@code body}, a living survivor on this account, has no id yet. */
    public synchronized boolean needsNew(String gameId, Object body) {
        Entry entry = entries().get(gameId);
        if (entry == null) {
            return true;
        }
        return entry.ended && (entry.deadBody == null || entry.deadBody.get() != body);
    }

    public synchronized GameCharacter begin(String gameId, String name) {
        Entry entry = new Entry();
        entry.id = UUID.randomUUID().toString();
        entry.name = name;
        entries().put(gameId, entry);
        save();
        return new GameCharacter(entry.id, entry.name);
    }

    /** The account's survivor, or null while there is none: before the first spawn and after a death. */
    public synchronized GameCharacter current(String gameId) {
        Entry entry = entries().get(gameId);
        return entry == null || entry.ended ? null : new GameCharacter(entry.id, entry.name);
    }

    /** Records the death of {@code body} and returns the survivor it ended, or null if none was known. */
    public synchronized GameCharacter end(String gameId, Object body) {
        GameCharacter ended = current(gameId);
        if (ended != null) {
            Entry entry = entries().get(gameId);
            entry.ended = true;
            entry.deadBody = new WeakReference<>(body);
            save();
        }
        return ended;
    }

    private Map<String, Entry> entries() {
        if (file == null) {
            file = fileSource.get();
            load();
        }
        return entries;
    }

    private void load() {
        if (!Files.isRegularFile(file)) {
            return;
        }
        try {
            Map<String, Entry> stored = GSON.fromJson(Files.readString(file, StandardCharsets.UTF_8), MAP_TYPE);
            if (stored != null) {
                stored.forEach((gameId, entry) -> {
                    if (entry != null && entry.id != null && entry.name != null) {
                        entries.put(gameId, entry);
                    }
                });
            }
        } catch (IOException | RuntimeException e) {
            AgentLog.error("characters: could not read " + file, e);
        }
    }

    private void save() {
        Path temp = file.resolveSibling(file.getFileName() + ".tmp");
        try {
            Files.writeString(temp, GSON.toJson(entries, MAP_TYPE), StandardCharsets.UTF_8);
            Files.move(temp, file, StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
        } catch (IOException | RuntimeException e) {
            AgentLog.error("characters: could not write " + file, e);
        }
    }
}
