package dev.takaro.hytale.protocol;

/**
 * An action failure with a Takaro protocol v1 error code. On protocol 0 only the message is
 * used, in the old response shapes.
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

    /** The v1 code for any exception an action handler let escape. */
    public static String codeFor(Throwable error) {
        if (error instanceof ProtocolException pe) {
            return pe.code();
        }
        if (error instanceof IllegalArgumentException) {
            return INVALID_ARGS;
        }
        if (error instanceof UnsupportedOperationException) {
            return UNSUPPORTED;
        }
        return GAME_ERROR;
    }
}
