package io.takaro.zomboid.core.model;

/** One Progression Track; {@code name}, {@code experience} and {@code progress} may be null. */
public record ProgressionTrack(String key, String name, int level, Double experience, Double progress) {}
