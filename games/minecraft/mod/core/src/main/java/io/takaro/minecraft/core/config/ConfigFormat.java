package io.takaro.minecraft.core.config;

import io.takaro.minecraft.core.TakaroConfig;

/** One platform's config file format: Paper YAML, Fabric JSON or NeoForge properties. */
public interface ConfigFormat {

    /**
     * The values the file holds, without environment overrides or defaults for empty fields.
     * Throws when the text is not a complete config (a half-saved file, a typo), so the caller
     * keeps the settings it has instead of dropping the token.
     */
    TakaroConfig parse(String text) throws Exception;

    /** The commented file written when there is none. */
    String defaultText();

    /** {@code text} with the identity token set to {@code identity}, everything else kept. */
    String withIdentity(String text, String identity) throws Exception;
}
