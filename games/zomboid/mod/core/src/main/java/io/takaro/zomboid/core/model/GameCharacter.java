package io.takaro.zomboid.core.model;

/** A survivor as Takaro sees it: {@code id} lasts for the character's life and never comes from its name. */
public record GameCharacter(String id, String name) {
}
