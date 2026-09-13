package io.takaro.zomboid.core.model;

public record BanEntry(String gameId, String name, String reason, String expiresAt) {}
