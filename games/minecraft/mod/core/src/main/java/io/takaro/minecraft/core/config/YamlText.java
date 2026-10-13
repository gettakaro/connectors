package io.takaro.minecraft.core.config;

import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** Text edits on Paper's {@code config.yml} that keep its comments and layout. */
public final class YamlText {

    private static final Pattern EMPTY_IDENTITY = Pattern.compile(
            "(?m)^([ \\t]*identity_token[ \\t]*:)[ \\t]*(?:\"\"|''|~|null)?[ \\t]*(#.*)?$");

    private YamlText() {}

    /** {@code text} with an empty {@code identity_token:} filled in, or null when there is no such line. */
    public static String withIdentity(String text, String identity) {
        Matcher m = EMPTY_IDENTITY.matcher(text);
        if (!m.find()) {
            return null;
        }
        String comment = m.group(2) == null ? "" : " " + m.group(2);
        return text.substring(0, m.start()) + m.group(1) + " \"" + identity + "\"" + comment + text.substring(m.end());
    }
}
