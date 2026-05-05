package dev.takaro.hytale.handlers;

import com.hypixel.hytale.server.core.Message;
import com.hypixel.hytale.server.core.event.events.player.PlayerChatEvent;
import com.hypixel.hytale.server.core.universe.PlayerRef;

import java.awt.Color;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public class ChatFormatter {
    // Named color mappings
    private static final Map<String, Color> NAMED_COLORS = new HashMap<>();

    static {
        NAMED_COLORS.put("red", Color.RED);
        NAMED_COLORS.put("green", Color.GREEN);
        NAMED_COLORS.put("blue", Color.BLUE);
        NAMED_COLORS.put("yellow", Color.YELLOW);
        NAMED_COLORS.put("orange", Color.ORANGE);
        NAMED_COLORS.put("pink", Color.PINK);
        NAMED_COLORS.put("white", Color.WHITE);
        NAMED_COLORS.put("black", Color.BLACK);
        NAMED_COLORS.put("gray", Color.GRAY);
        NAMED_COLORS.put("grey", Color.GRAY);
        NAMED_COLORS.put("cyan", Color.CYAN);
        NAMED_COLORS.put("magenta", Color.MAGENTA);
        NAMED_COLORS.put("purple", new Color(128, 0, 128));
        NAMED_COLORS.put("gold", new Color(255, 215, 0));
        NAMED_COLORS.put("lime", new Color(0, 255, 0));
        NAMED_COLORS.put("aqua", new Color(0, 255, 255));
    }

    public static void onPlayerChat(PlayerChatEvent event) {
        PlayerRef sender = event.getSender();
        String content = event.getContent();

        // Parse color codes in player messages
        if (content.contains("[") && content.contains("]")) {
            Message formattedMessage = parseColoredMessage(content);
            if (formattedMessage != null) {
                event.setFormatter((playerRef, message) ->
                    Message.join(
                        Message.raw("<").color(Color.GRAY),
                        Message.raw(sender.getUsername()).color(Color.WHITE),
                        Message.raw("> ").color(Color.GRAY),
                        formattedMessage
                    )
                );
            }
        }
    }

    /**
     * Parses a message with color codes like [ff0000]text[-] or [red]text[-]
     * Returns a formatted Message object with colors applied
     */
    public static Message parseColoredMessage(String input) {
        if (input == null || input.isEmpty()) {
            return Message.raw(input);
        }

        // Pattern to match [colorcode]text[-]
        // Supports hex like [ff0000] or named like [red]
        Pattern pattern = Pattern.compile("\\[([a-fA-F0-9]{6}|[a-zA-Z]+)\\](.*?)\\[-\\]");
        Matcher matcher = pattern.matcher(input);

        List<Message> parts = new ArrayList<>();
        int lastEnd = 0;

        while (matcher.find()) {
            // Add text before the color code as plain text
            if (matcher.start() > lastEnd) {
                String beforeText = input.substring(lastEnd, matcher.start());
                if (!beforeText.isEmpty()) {
                    parts.add(Message.raw(beforeText));
                }
            }

            String colorCode = matcher.group(1);
            String coloredText = matcher.group(2);

            Color color = parseColor(colorCode);
            if (color != null) {
                parts.add(Message.raw(coloredText).color(color));
            } else {
                // If color parsing failed, just add as plain text
                parts.add(Message.raw(coloredText));
            }

            lastEnd = matcher.end();
        }

        // Add remaining text after last match
        if (lastEnd < input.length()) {
            String remainingText = input.substring(lastEnd);
            if (!remainingText.isEmpty()) {
                parts.add(Message.raw(remainingText));
            }
        }

        if (parts.isEmpty()) {
            return Message.raw(input);
        } else if (parts.size() == 1) {
            return parts.get(0);
        } else {
            return Message.join(parts.toArray(new Message[0]));
        }
    }

    /**
     * Parses a color code - supports hex (ff0000) or named colors (red)
     */
    public static Color parseColor(String colorCode) {
        if (colorCode == null || colorCode.isEmpty()) {
            return null;
        }

        colorCode = colorCode.toLowerCase();

        // Check if it's a named color
        if (NAMED_COLORS.containsKey(colorCode)) {
            return NAMED_COLORS.get(colorCode);
        }

        // Try parsing as hex
        if (colorCode.length() == 6) {
            try {
                int r = Integer.parseInt(colorCode.substring(0, 2), 16);
                int g = Integer.parseInt(colorCode.substring(2, 4), 16);
                int b = Integer.parseInt(colorCode.substring(4, 6), 16);
                return new Color(r, g, b);
            } catch (NumberFormatException e) {
                return null;
            }
        }

        return null;
    }

    /**
     * Parse message from Takaro with color codes AND clickable links
     * Combines color parsing with URL detection for Takaro messages
     */
    public static Message parseTakaroMessage(String input) {
        if (input == null || input.isEmpty()) {
            return Message.raw(input);
        }

        // Pattern to match [colorcode]text[-]
        Pattern colorPattern = Pattern.compile("\\[([a-fA-F0-9]{6}|[a-zA-Z]+)\\](.*?)\\[-\\]");
        Matcher colorMatcher = colorPattern.matcher(input);

        List<Message> parts = new ArrayList<>();
        int lastEnd = 0;

        while (colorMatcher.find()) {
            // Add text before the color code, parsing URLs
            if (colorMatcher.start() > lastEnd) {
                String beforeText = input.substring(lastEnd, colorMatcher.start());
                if (!beforeText.isEmpty()) {
                    parts.addAll(parseLinksInText(beforeText));
                }
            }

            String colorCode = colorMatcher.group(1);
            String coloredText = colorMatcher.group(2);

            Color color = parseColor(colorCode);
            if (color != null) {
                // Parse URLs within colored text
                List<Message> coloredParts = parseLinksInText(coloredText);
                for (Message part : coloredParts) {
                    // Apply color to each part
                    parts.add(part.color(color));
                }
            } else {
                // If color parsing failed, parse URLs in text
                parts.addAll(parseLinksInText(coloredText));
            }

            lastEnd = colorMatcher.end();
        }

        // Add remaining text after last match, parsing URLs
        if (lastEnd < input.length()) {
            String remainingText = input.substring(lastEnd);
            if (!remainingText.isEmpty()) {
                parts.addAll(parseLinksInText(remainingText));
            }
        }

        if (parts.isEmpty()) {
            return Message.raw(input);
        } else if (parts.size() == 1) {
            return parts.get(0);
        } else {
            return Message.join(parts.toArray(new Message[0]));
        }
    }

    /**
     * Parse URLs in text and return Message parts with clickable links
     */
    private static List<Message> parseLinksInText(String text) {
        List<Message> parts = new ArrayList<>();

        // Pattern to match URLs (http://, https://, or www.)
        Pattern urlPattern = Pattern.compile("(https?://[^\\s]+|www\\.[^\\s]+)");
        Matcher urlMatcher = urlPattern.matcher(text);

        int lastEnd = 0;

        while (urlMatcher.find()) {
            // Add text before URL as plain text
            if (urlMatcher.start() > lastEnd) {
                String beforeText = text.substring(lastEnd, urlMatcher.start());
                if (!beforeText.isEmpty()) {
                    parts.add(Message.raw(beforeText));
                }
            }

            String url = urlMatcher.group(1);
            String linkUrl = url;

            // If URL starts with www., add https:// for the link
            if (url.startsWith("www.")) {
                linkUrl = "https://" + url;
            }

            // Create clickable link in cyan color
            parts.add(Message.raw(url).link(linkUrl).color("#00BFFF"));

            lastEnd = urlMatcher.end();
        }

        // Add remaining text after last URL
        if (lastEnd < text.length()) {
            String remainingText = text.substring(lastEnd);
            if (!remainingText.isEmpty()) {
                parts.add(Message.raw(remainingText));
            }
        }

        // If no URLs found, return the whole text as one part
        if (parts.isEmpty()) {
            parts.add(Message.raw(text));
        }

        return parts;
    }
}
