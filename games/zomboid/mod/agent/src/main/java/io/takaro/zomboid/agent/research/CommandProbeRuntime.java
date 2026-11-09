package io.takaro.zomboid.agent.research;

import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import com.google.gson.JsonParser;

import io.takaro.zomboid.agent.AgentLog;
import io.takaro.zomboid.agent.MainThreadQueue;
import io.takaro.zomboid.agent.TakaroPaths;

import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

import zombie.characters.Role;
import zombie.commands.CommandBase;
import zombie.core.network.ByteBufferReader;
import zombie.core.raknet.UdpConnection;
import zombie.network.GameServer;
import zombie.network.chat.ChatServer;

/**
 * Game side of the #201 command probe: the stub backend over HTTP, the catalog poller and the
 * packet peek. Created only when {@code TAKARO_COMMAND_PROBE_URL} is set.
 */
public final class CommandProbeRuntime {

    public static final String URL_ENV = "TAKARO_COMMAND_PROBE_URL";
    private static final Duration POLL = Duration.ofSeconds(2);
    private static final Duration INVOKE_TIMEOUT = Duration.ofSeconds(3);

    private static volatile CommandProbeRuntime instance;

    private final String baseUrl;
    private final HttpClient http = HttpClient.newBuilder().connectTimeout(Duration.ofSeconds(2)).build();
    private final CommandProbe probe;
    private final MainThreadQueue queue;
    private final Path receiptFile;
    private final AtomicBoolean firstPacket = new AtomicBoolean();
    private boolean wasOnline; // main thread only

    private CommandProbeRuntime(String baseUrl, MainThreadQueue queue) {
        this.baseUrl = baseUrl.endsWith("/") ? baseUrl.substring(0, baseUrl.length() - 1) : baseUrl;
        this.queue = queue;
        this.receiptFile = TakaroPaths.configFile().resolveSibling("command-probe-receipts.jsonl");
        this.probe = new CommandProbe(CommandProbeRuntime::isNativeCommand, this::invoke, this::reply,
                this::writeReceipt, new ThreadPoolExecutor(4, 4, 0L, TimeUnit.MILLISECONDS,
                        new ArrayBlockingQueue<>(16), r -> daemon(r, "takaro-command-probe")));
    }

    public static boolean enabled() {
        String url = System.getenv(URL_ENV);
        return url != null && !url.isBlank();
    }

    /** Called once from premain when the flag is set. */
    public static void start(MainThreadQueue queue) {
        CommandProbeRuntime rt = new CommandProbeRuntime(System.getenv(URL_ENV).strip(), queue);
        instance = rt;
        daemon(rt::pollLoop, "takaro-command-probe-catalog").start();
        AgentLog.log("command-probe: enabled, stub=" + rt.baseUrl + " receipts=" + rt.receiptFile);
    }

    /**
     * {@code GameServer.receiveReceiveCommand} entry. Reads the command string, and when the probe
     * does not claim it puts the buffer back exactly where it was for the game's own read.
     */
    public static boolean onReceiveCommand(Object reader, Object connection) {
        CommandProbeRuntime rt = instance;
        if (rt == null) {
            return false;
        }
        ByteBufferReader bb = (ByteBufferReader) reader;
        UdpConnection c = (UdpConnection) connection;
        int position = bb.position();
        boolean claimed = false;
        try {
            String input = bb.getUTF();
            if (rt.firstPacket.compareAndSet(false, true)) {
                AgentLog.log("command-probe: HOOK CONFIRMED receiveReceiveCommand thread="
                        + Thread.currentThread().getName());
            }
            claimed = rt.probe.claim(input, c, caller(c));
            // Only the name: arguments can carry secrets (for example /changepwd).
            String name = CommandProbe.commandName(input);
            AgentLog.log("command-probe: input from " + c.getUserName() + " "
                    + (claimed ? "CLAIMED" : "passed to game") + ": /" + (name == null ? "" : name));
            return claimed;
        } finally {
            if (!claimed) {
                bb.position(position);
            }
        }
    }

    static CommandProbe.Caller caller(UdpConnection c) {
        Role role = c.getRole();
        int local = 0;
        if (c.players != null) {
            for (Object p : c.players) {
                if (p != null) {
                    local++;
                }
            }
        }
        return new CommandProbe.Caller(c.getUserName(), c.getSteamId(),
                role == null ? null : role.getName(), local, c.getConnectedGUID());
    }

    private static boolean isNativeCommand(String name) {
        return CommandBase.findCommandCls(name) != null;
    }

    private void reply(Object connection, String text) {
        queue.runOnMainThread(() -> {
            UdpConnection c = (UdpConnection) connection;
            if (GameServer.udpEngine == null || !GameServer.udpEngine.connections.contains(c)
                    || !c.isFullyConnected()) {
                AgentLog.log("command-probe: caller gone, reply dropped: " + text);
                return;
            }
            ChatServer.getInstance().sendMessageToServerChat(c, text);
            AgentLog.log("command-probe: replied to " + c.getUserName() + " only: " + text);
        });
    }

    private CommandProbe.Outcome invoke(JsonObject invocation) throws Exception {
        HttpRequest req = HttpRequest.newBuilder(URI.create(baseUrl + "/invoke"))
                .timeout(INVOKE_TIMEOUT)
                .header("content-type", "application/json")
                .POST(HttpRequest.BodyPublishers.ofString(invocation.toString()))
                .build();
        HttpResponse<String> res = http.send(req, HttpResponse.BodyHandlers.ofString());
        if (res.statusCode() != 200) {
            throw new IllegalStateException("HTTP " + res.statusCode());
        }
        JsonObject body = JsonParser.parseString(res.body()).getAsJsonObject();
        List<String> replies = new ArrayList<>();
        if (body.has("replies")) {
            for (JsonElement e : body.getAsJsonArray("replies")) {
                replies.add(e.getAsString());
            }
        }
        return new CommandProbe.Outcome(body.get("status").getAsString(), replies);
    }

    private void pollLoop() {
        while (true) {
            JsonObject catalog = null;
            try {
                HttpRequest req = HttpRequest.newBuilder(URI.create(baseUrl + "/catalog"))
                        .timeout(Duration.ofSeconds(2)).GET().build();
                HttpResponse<String> res = http.send(req, HttpResponse.BodyHandlers.ofString());
                if (res.statusCode() == 200) {
                    catalog = JsonParser.parseString(res.body()).getAsJsonObject();
                }
            } catch (InterruptedException e) {
                return;
            } catch (Exception e) {
                catalog = null;
            }
            // Applied on the main thread: the native-name check reads the game's command registry.
            JsonObject fetched = catalog;
            queue.runOnMainThread(() -> applyOnMainThread(fetched));
            try {
                Thread.sleep(POLL.toMillis());
            } catch (InterruptedException e) {
                return;
            }
        }
    }

    private void applyOnMainThread(JsonObject catalog) {
        CommandProbe.Snapshot applied = probe.applyCatalog(catalog);
        boolean nowOnline = probe.online();
        if (nowOnline != wasOnline) {
            AgentLog.log("command-probe: backend " + (nowOnline ? "online" : "OFFLINE")
                    + " (catalog revision " + probe.snapshot().revision() + " kept)");
            wasOnline = nowOnline;
        }
        if (applied != null) {
            AgentLog.log("command-probe: catalog revision " + applied.revision()
                    + " names=" + applied.byName().keySet() + " refused=" + applied.refused());
        }
    }

    private synchronized void writeReceipt(JsonObject receipt) {
        try {
            Files.writeString(receiptFile, receipt + "\n", StandardCharsets.UTF_8,
                    StandardOpenOption.CREATE, StandardOpenOption.APPEND);
        } catch (Exception e) {
            AgentLog.error("command-probe: receipt write failed", e);
        }
        AgentLog.log("command-probe: receipt " + receipt);
    }

    private static Thread daemon(Runnable r, String name) {
        Thread t = new Thread(r, name);
        t.setDaemon(true);
        return t;
    }
}
