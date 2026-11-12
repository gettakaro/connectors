package io.takaro.zomboid.core.model;

/** One answer to {@code getItemIcons}: the PNG bytes behind a hash. */
public record ItemIcon(String hash, byte[] png) {}
