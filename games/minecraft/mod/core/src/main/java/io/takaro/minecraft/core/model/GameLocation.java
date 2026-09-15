package io.takaro.minecraft.core.model;

public record GameLocation(
        String name,
        String code,
        double x, double y, double z,
        String dimension,
        Double radius,
        Double sizeX, Double sizeY, Double sizeZ
) {}
