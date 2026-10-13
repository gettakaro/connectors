package io.takaro.zomboid.core.model;

/** The protocol v1 {@code image} map source: one encoded file Takaro fetches in chunks. */
public record MapImageInfo(String revision, long size, int width, int height, String mimeType) {
}
