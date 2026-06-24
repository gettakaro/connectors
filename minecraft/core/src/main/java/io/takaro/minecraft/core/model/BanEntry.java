package io.takaro.minecraft.core.model;

public record BanEntry(String gameId, String name, String reason, String expiresAt) {}
