package io.takaro.minecraft.core.model;

public record CommandResult(boolean success, String rawResult, String errorMessage) {}
