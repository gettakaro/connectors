package io.takaro.zomboid.core.model;

public record CommandResult(boolean success, String rawResult, String errorMessage) {}
