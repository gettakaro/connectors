package io.takaro.zomboid.core.model;

/** Bytes {@code offset..offset+data.length-1} of the map image with the given revision. */
public record MapImageChunk(String revision, byte[] data) {
}
