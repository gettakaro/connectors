package io.takaro.minecraft.core.model;

/**
 * A message from Takaro as the player sees it. With no sender it is the bare text, as before. With
 * a sender it uses the game's own server-message formats: the {@code /say} line {@code [Name] text}
 * in global chat and the {@code /msg} line {@code Name whispers to you: text} (grey, italic) in a
 * directed message. Platforms build a translatable component from {@link #translationKey()} with
 * the arguments {@code sender, message}, and {@link #plainText()} is its English fallback.
 */
public record ChatLine(String message, String sender, boolean directed) {

    /** Vanilla {@code /say}: {@code [%s] %s}. */
    public static final String GLOBAL_KEY = "chat.type.announcement";
    /** Vanilla {@code /msg}, as the recipient sees it: {@code %s whispers to you: %s}. */
    public static final String DIRECTED_KEY = "commands.message.display.incoming";

    public boolean hasSender() {
        return sender != null;
    }

    /** True when the line is styled like a vanilla whisper (grey, italic). */
    public boolean whisper() {
        return hasSender() && directed;
    }

    public String translationKey() {
        return directed ? DIRECTED_KEY : GLOBAL_KEY;
    }

    public String plainText() {
        if (!hasSender()) return message;
        return directed ? sender + " whispers to you: " + message : "[" + sender + "] " + message;
    }
}
