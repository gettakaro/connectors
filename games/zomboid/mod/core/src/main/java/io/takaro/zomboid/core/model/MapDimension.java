package io.takaro.zomboid.core.model;

/**
 * One map Dimension for the protocol v1 {@code map} capability: which position fields span
 * the screen and which way they grow, the world rectangle the map covers (raw game
 * coordinates), and the image that covers exactly that rectangle ({@code null} = grid only).
 */
public record MapDimension(
        String id,
        String name,
        String horizontalField,
        String horizontalDirection,
        String verticalField,
        String verticalDirection,
        double horizontalMin,
        double horizontalMax,
        double verticalMin,
        double verticalMax,
        MapImageInfo image) {
}
