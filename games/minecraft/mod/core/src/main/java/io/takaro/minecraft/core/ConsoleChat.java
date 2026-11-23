package io.takaro.minecraft.core;

/** Finds the text of a /say, for loaders that only hand out the raw command line. */
public final class ConsoleChat {

    private ConsoleChat() {}

    /** The message of a /say command line, or null when the line is anything else. */
    public static String sayText(String commandLine) {
        if (commandLine == null) return null;
        String line = commandLine.strip();
        if (line.startsWith("/")) line = line.substring(1);
        if (line.startsWith("minecraft:")) line = line.substring("minecraft:".length());
        if (!line.startsWith("say ")) return null;
        String text = line.substring(4).strip();
        return text.isEmpty() ? null : text;
    }
}
