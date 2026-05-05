package dev.takaro.hytale.handlers;

import com.hypixel.hytale.server.core.Message;
import com.hypixel.hytale.server.core.command.system.CommandSender;

import javax.annotation.Nonnull;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;

public class OutputCapturingCommandSender implements CommandSender {
    private final List<String> capturedMessages = new ArrayList<>();
    private final UUID uuid = new UUID(0L, 0L);

    @Override
    public void sendMessage(@Nonnull Message message) {
        String text = message.getAnsiMessage();
        if (text != null && !text.isEmpty()) {
            capturedMessages.add(text);
        }
    }

    @Nonnull
    @Override
    public String getDisplayName() {
        return "Takaro Console";
    }

    @Nonnull
    @Override
    public UUID getUuid() {
        return this.uuid;
    }

    @Override
    public boolean hasPermission(@Nonnull String id) {
        return true;
    }

    @Override
    public boolean hasPermission(@Nonnull String id, boolean def) {
        return true;
    }

    public List<String> getCapturedMessages() {
        return capturedMessages;
    }

    public String getCapturedOutput() {
        return String.join("\n", capturedMessages);
    }
}
