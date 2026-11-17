package io.takaro.zomboid.core.model;

/** One online player's server-observed position in a pushed position batch. */
public record PlayerPositionSample(String gameId, PlayerLocation position) {}
