package io.takaro.zomboid.core;

/**
 * An action failure with a protocol v1 error code ({@code unsupported}, {@code invalid_args},
 * {@code not_found}, {@code game_error}, {@code internal}). On protocol 0 only the message is
 * sent, as before.
 */
public class ProtocolException extends RuntimeException {

    public static final String UNSUPPORTED = "unsupported";
    public static final String INVALID_ARGS = "invalid_args";
    public static final String NOT_FOUND = "not_found";
    public static final String GAME_ERROR = "game_error";
    public static final String INTERNAL = "internal";

    private final String code;

    public ProtocolException(String code, String message) {
        super(message);
        this.code = code;
    }

    public String code() {
        return code;
    }
}
