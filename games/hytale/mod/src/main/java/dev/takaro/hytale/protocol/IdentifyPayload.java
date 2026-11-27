package dev.takaro.hytale.protocol;

import com.google.gson.JsonElement;
import com.google.gson.JsonObject;

import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * The protocol fields of {@code identify} (everything except the tokens) and the reading of
 * Takaro's choice in {@code identifyResponse}.
 */
public final class IdentifyPayload {
    public static final List<Integer> OFFERED_VERSIONS = List.of(0, 1);

    private IdentifyPayload() {
    }

    /**
     * {@code game} and {@code connector} go out on both versions; the manifest is only read by
     * a Takaro that picks protocol 1, and an older Takaro ignores it.
     */
    public static Map<String, Object> build(String connectorVersion, String rawGameVersion,
                                            Map<String, Object> mapDimension) {
        Map<String, Object> out = new LinkedHashMap<>();
        out.put("game", GameProfile.GAME);
        Map<String, Object> connector = GameProfile.connector(connectorVersion);
        if (connector != null) {
            out.put("connector", connector);
        }
        out.put("protocolVersions", OFFERED_VERSIONS);
        out.put("capabilities", Capabilities.manifest(rawGameVersion, mapDimension));
        return out;
    }

    /** The protocol Takaro chose; a missing or unreadable value means 0, as an older Takaro sends. */
    public static int chosenProtocol(JsonObject identifyResponsePayload) {
        if (identifyResponsePayload == null) {
            return 0;
        }
        JsonElement v = identifyResponsePayload.get("protocolVersion");
        if (v == null || !v.isJsonPrimitive() || !v.getAsJsonPrimitive().isNumber()) {
            return 0;
        }
        int chosen = v.getAsInt();
        return OFFERED_VERSIONS.contains(chosen) ? chosen : 0;
    }
}
