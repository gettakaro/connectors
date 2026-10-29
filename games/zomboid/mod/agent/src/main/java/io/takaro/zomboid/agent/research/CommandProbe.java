package io.takaro.zomboid.agent.research;

import com.google.gson.Gson;
import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;

import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.Predicate;
import java.util.regex.Pattern;

/**
 * Research probe for issue #201: claims slash input that names a command in a stub catalog
 * and answers only the connection that sent it. Not the Takaro command protocol; nothing here
 * runs unless {@code TAKARO_COMMAND_PROBE_URL} is set.
 *
 * <p>Game-free so it can be unit tested: the game side supplies the caller, a native-command
 * lookup and a {@link Replier}; the backend is the local stub.
 */
public final class CommandProbe {

    /** Names Takaro may claim: one typable token. */
    static final Pattern NAME = Pattern.compile("^[A-Za-z0-9_]{1,32}$");

    /**
     * Names the game owns before the server's native registry is consulted. The client turns
     * the first set into chat streams and never sends them as commands (ISChat.allChatStreams);
     * the server answers the second set in GameServer.handleClientCommand.
     */
    static final Set<String> CLIENT_STREAMS = Set.of(
            "say", "s", "yell", "y", "whisper", "w", "faction", "f", "safehouse", "sh", "all", "admin", "a");
    static final Set<String> CLIENT_COMMANDS = Set.of(
            "card", "roll", "changepwd", "dragons", "dance", "safehouse");

    public static final String UNAVAILABLE = "Takaro is unavailable right now. Your command was not run.";
    public static final String UNKNOWN_OUTCOME = "Takaro did not answer. Your command may or may not have run.";
    public static final String MALFORMED = "Takaro could not read that command: a quote is not closed.";
    public static final String AMBIGUOUS = "Takaro cannot tell which local player sent that command. It was not run.";
    public static final String BUSY = "Takaro is busy right now. Your command was not run.";

    /** Who sent the input, as the receiving connection reports it. */
    public record Caller(String username, long steamId, String role, int localPlayers, long connectionGuid) {
    }

    /** One catalog row. Several rows may share a name: that is backend fanout, kept as is. */
    public record Entry(String name, String installationId) {
    }

    /** An immutable catalog revision, keyed by lower-case name. */
    public record Snapshot(long revision, Map<String, List<Entry>> byName, List<String> refused) {
        static final Snapshot EMPTY = new Snapshot(-1, Map.of(), List.of());
    }

    /** What the backend answered for one invocation. */
    public record Outcome(String status, List<String> replies) {
    }

    public interface Backend {
        Outcome invoke(JsonObject invocation) throws Exception;
    }

    public interface Replier {
        /** Delivers one private line to the connection, on the game's main thread. */
        void reply(Object connection, String text);
    }

    public interface ReceiptLog {
        void write(JsonObject receipt);
    }

    private static final Gson GSON = new Gson();

    private final Predicate<String> nativeCommand;
    private final Backend backend;
    private final Replier replier;
    private final ReceiptLog receipts;
    private final ExecutorService workers;
    private final AtomicLong sequence = new AtomicLong();

    private volatile Snapshot snapshot = Snapshot.EMPTY;
    private volatile boolean online;

    public CommandProbe(Predicate<String> nativeCommand, Backend backend, Replier replier,
                        ReceiptLog receipts, ExecutorService workers) {
        this.nativeCommand = nativeCommand;
        this.backend = backend;
        this.replier = replier;
        this.receipts = receipts;
        this.workers = workers;
    }

    public Snapshot snapshot() {
        return snapshot;
    }

    public boolean online() {
        return online;
    }

    /**
     * Applies a catalog poll result. A failed poll marks the backend offline but keeps the last
     * snapshot, so a recognized command still fails privately instead of reaching vanilla.
     * Older revisions are ignored. Returns the applied snapshot, or null when nothing changed.
     */
    public synchronized Snapshot applyCatalog(JsonObject catalog) {
        Snapshot next;
        try {
            next = catalog == null ? null : build(catalog, nativeCommand);
        } catch (RuntimeException malformed) {
            next = null;
        }
        if (next == null) {
            online = false;
            return null;
        }
        online = true;
        if (next.revision() <= snapshot.revision()) {
            return null;
        }
        snapshot = next;
        return next;
    }

    static Snapshot build(JsonObject catalog, Predicate<String> nativeCommand) {
        long revision = catalog.has("revision") ? catalog.get("revision").getAsLong() : 0L;
        Map<String, List<Entry>> byName = new LinkedHashMap<>();
        List<String> refused = new ArrayList<>();
        JsonArray rows = catalog.has("commands") && catalog.get("commands").isJsonArray()
                ? catalog.getAsJsonArray("commands") : new JsonArray();
        for (JsonElement row : rows) {
            if (!row.isJsonObject() || !row.getAsJsonObject().has("name")) {
                continue;
            }
            JsonObject o = row.getAsJsonObject();
            String name = o.get("name").getAsString();
            String install = o.has("installationId") ? o.get("installationId").getAsString() : "";
            String key = name.toLowerCase(Locale.ROOT);
            String why = refusal(name, key, nativeCommand);
            if (why != null) {
                refused.add(name + ": " + why);
                continue;
            }
            byName.computeIfAbsent(key, k -> new ArrayList<>()).add(new Entry(name, install));
        }
        byName.replaceAll((k, v) -> List.copyOf(v));
        return new Snapshot(revision, Collections.unmodifiableMap(byName), List.copyOf(refused));
    }

    private static String refusal(String name, String key, Predicate<String> nativeCommand) {
        if (!NAME.matcher(name).matches()) {
            return "not one token";
        }
        if (CLIENT_STREAMS.contains(key)) {
            return "client chat stream";
        }
        if (CLIENT_COMMANDS.contains(key)) {
            return "game client command";
        }
        // The server echoes any reply to /roll... or /card... to every player.
        if (key.startsWith("roll") || key.startsWith("card")) {
            return "reply would be public";
        }
        if (nativeCommand.test(key)) {
            return "native server command";
        }
        return null;
    }

    /** The command name of slash input (without the slash), or null when it is not a command. */
    static String commandName(String input) {
        if (input == null || !input.startsWith("/")) {
            return null;
        }
        String body = input.substring(1).stripLeading();
        int end = 0;
        while (end < body.length() && !Character.isWhitespace(body.charAt(end))) {
            end++;
        }
        return end == 0 ? null : body.substring(0, end);
    }

    /**
     * Splits arguments the way GameServer.handleClientCommand does (a token or a quoted run),
     * or returns null when a quote is left open.
     */
    static List<String> arguments(String input) {
        String name = commandName(input);
        String rest = input.substring(1).stripLeading().substring(name.length()).strip();
        List<String> out = new ArrayList<>();
        int i = 0;
        while (i < rest.length()) {
            char c = rest.charAt(i);
            if (Character.isWhitespace(c)) {
                i++;
            } else if (c == '"') {
                int close = rest.indexOf('"', i + 1);
                if (close < 0) {
                    return null;
                }
                out.add(rest.substring(i + 1, close));
                i = close + 1;
            } else {
                int end = i;
                while (end < rest.length() && !Character.isWhitespace(rest.charAt(end))) {
                    end++;
                }
                out.add(rest.substring(i, end));
                i = end;
            }
        }
        return out;
    }

    /**
     * Called on the packet thread with the raw input. Returns true when the input is claimed;
     * the game's own handling is then skipped and the answer is delivered later through the
     * {@link Replier}. Unknown input returns false and the game handles it as before.
     */
    public boolean claim(String input, Object connection, Caller caller) {
        String name = commandName(input);
        if (name == null) {
            return false;
        }
        Snapshot snap = snapshot;
        List<Entry> matches = snap.byName().get(name.toLowerCase(Locale.ROOT));
        if (matches == null) {
            return false;
        }
        long seq = sequence.incrementAndGet();
        boolean backendOnline = online;
        try {
            workers.execute(() -> run(seq, input, name, matches, snap.revision(), backendOnline, connection, caller));
        } catch (RejectedExecutionException full) {
            // Never queue without bound: a full pool fails the command privately, unsent.
            replier.reply(connection, BUSY);
        }
        return true;
    }

    private void run(long seq, String input, String name, List<Entry> matches, long revision,
                     boolean backendOnline, Object connection, Caller caller) {
        JsonObject receipt = new JsonObject();
        receipt.addProperty("seq", seq);
        receipt.addProperty("at", System.currentTimeMillis());
        receipt.addProperty("command", name);
        receipt.addProperty("catalogRevision", revision);
        receipt.add("caller", GSON.toJsonTree(caller));
        JsonArray installs = new JsonArray();
        matches.forEach(m -> installs.add(m.installationId()));
        receipt.add("installations", installs);
        List<String> replies;
        String outcome;
        List<String> args = arguments(input);
        if (args == null) {
            outcome = "malformed";
            replies = List.of(MALFORMED);
        } else if (caller.localPlayers() > 1) {
            outcome = "ambiguous-caller";
            replies = List.of(AMBIGUOUS);
        } else if (!backendOnline) {
            outcome = "offline";
            replies = List.of(UNAVAILABLE);
        } else {
            JsonObject invocation = new JsonObject();
            invocation.addProperty("seq", seq);
            invocation.addProperty("command", name);
            invocation.add("args", GSON.toJsonTree(args));
            invocation.add("installations", installs);
            invocation.add("caller", GSON.toJsonTree(caller));
            try {
                Outcome result = backend.invoke(invocation);
                outcome = result.status();
                replies = result.replies().isEmpty() ? List.of("Done.") : result.replies();
            } catch (Exception e) {
                outcome = "no-answer:" + e.getClass().getSimpleName();
                replies = List.of(UNKNOWN_OUTCOME);
            }
        }
        receipt.add("args", args == null ? null : GSON.toJsonTree(args));
        receipt.addProperty("outcome", outcome);
        receipt.addProperty("reply", String.join(" | ", replies));
        receipts.write(receipt);
        // One private line per invocation, even when several installations answered.
        replier.reply(connection, String.join(" | ", replies));
    }
}
