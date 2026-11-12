package io.takaro.zomboid.core.model;

/** {@code iconHash} is the SHA-256 of the PNG behind the item's art, or null when it has none. */
public record GameItem(String code, String name, String description, String iconHash) {

    public GameItem(String code, String name, String description) {
        this(code, name, description, null);
    }
}
