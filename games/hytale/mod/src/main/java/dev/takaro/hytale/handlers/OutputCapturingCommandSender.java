package dev.takaro.hytale.handlers;

import com.hypixel.hytale.server.core.Message;
import com.hypixel.hytale.server.core.command.system.CommandSender;
import com.hypixel.hytale.server.core.util.MessageUtil;

import dev.takaro.hytale.util.Responses;

import javax.annotation.Nonnull;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CopyOnWriteArrayList;

/**
 * A {@link CommandSender} that keeps everything the command writes back to it.
 *
 * <p>Using this instead of subscribing to the global server logger means the output of a
 * Takaro-issued command is exactly that command's output - no other player's or console
 * user's log lines can leak into it.
 *
 * <p>the message has to be rendered the way Hytale's own console renders it.
 * {@code Message.getAnsiMessage()} resolves the translation key and its params but <b>drops the
 * message's children</b>, and {@code Message.insert(...)} - which is how commands attach their
 * body - appends exactly there. That is why {@code /help} used to come back as nothing but its
 * header line: the two-column command list is a child of the header message, not a separate
 * log line. {@code ConsoleSender} avoids this by going through
 * {@code MessageUtil.toAnsiString(Message)}, which recurses into {@code getChildren()}; the
 * plain-text equivalent, {@code MessageUtil.formatMessageToPlainString(FormattedMessage)},
 * does the same without the jline styling, so that is what is used here.
 *
 * <p>Every {@code CommandSender} callback also registers the calling thread with the
 * command's {@link ThreadScopedLogCapture}, which is what makes thread-scoped log capture
 * possible for commands that log instead of replying.
 */
public class OutputCapturingCommandSender implements CommandSender {
    private final List<String> capturedMessages = new CopyOnWriteArrayList<>();
    private final UUID uuid = new UUID(0L, 0L);
    private final ThreadScopedLogCapture logCapture;

    public OutputCapturingCommandSender() {
        this(null);
    }

    public OutputCapturingCommandSender(ThreadScopedLogCapture logCapture) {
        this.logCapture = logCapture;
    }

    private void markThread() {
        if (logCapture != null) {
            logCapture.markCurrentThread();
        }
    }

    @Override
    public void sendMessage(@Nonnull Message message) {
        markThread();
        String text = render(message);
        if (text == null || text.isEmpty()) {
            return;
        }
        capturedMessages.add(stripAnsi(text));
    }

    /**
     * Render a {@link Message} to plain text exactly as the server console would, children
     * (i.e. {@code insert(...)} bodies) included.
     */
    static String render(Message message) {
        if (message == null) {
            return null;
        }
        try {
            String plain = MessageUtil.formatMessageToPlainString(message.getFormattedMessage());
            if (plain != null && !plain.isEmpty()) {
                return plain;
            }
        } catch (Throwable ignored) {
            // Fall through to the older, lossier accessors rather than losing the output.
        }
        String text = message.getAnsiMessage();
        if (text == null || text.isEmpty()) {
            text = message.getRawText();
        }
        return text;
    }

    static String stripAnsi(String text) {
        return Responses.stripAnsi(text);
    }

    @Nonnull
    @Override
    public String getUsername() {
        markThread();
        return "Takaro Console";
    }

    @Nonnull
    @Override
    public UUID getUuid() {
        markThread();
        return this.uuid;
    }

    @Override
    public boolean hasPermission(@Nonnull String id) {
        markThread();
        return true;
    }

    @Override
    public boolean hasPermission(@Nonnull String id, boolean def) {
        markThread();
        return true;
    }

    public List<String> getCapturedMessages() {
        return capturedMessages;
    }

    public String getCapturedOutput() {
        return String.join("\n", capturedMessages);
    }
}
