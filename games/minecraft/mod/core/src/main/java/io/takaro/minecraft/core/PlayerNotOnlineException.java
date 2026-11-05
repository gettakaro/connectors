package io.takaro.minecraft.core;

/**
 * Thrown by a {@link GameAdapter} action that needs the player in the world when they are not.
 * Takaro gets {@code not_found} instead of a success that changed nothing.
 */
public class PlayerNotOnlineException extends RuntimeException {

    public PlayerNotOnlineException(String gameId) {
        super("player " + gameId + " is not online");
    }
}
