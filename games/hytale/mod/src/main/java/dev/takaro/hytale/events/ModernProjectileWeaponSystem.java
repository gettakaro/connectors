package dev.takaro.hytale.events;

import com.hypixel.hytale.component.CommandBuffer;
import com.hypixel.hytale.component.ComponentType;
import com.hypixel.hytale.component.Ref;
import com.hypixel.hytale.component.Store;
import com.hypixel.hytale.component.query.Query;
import com.hypixel.hytale.component.system.RefChangeSystem;
import com.hypixel.hytale.server.core.modules.projectile.config.StandardPhysicsProvider;
import com.hypixel.hytale.server.core.universe.world.storage.EntityStore;

import javax.annotation.Nonnull;

/** Captures the firing item for Hytale's current bow and crossbow projectile API. */
public class ModernProjectileWeaponSystem extends RefChangeSystem<EntityStore, StandardPhysicsProvider> {
    private final ProjectileWeaponSystem weapons;

    public ModernProjectileWeaponSystem(ProjectileWeaponSystem weapons) {
        this.weapons = weapons;
    }

    @Nonnull
    @Override
    public ComponentType<EntityStore, StandardPhysicsProvider> componentType() {
        return StandardPhysicsProvider.getComponentType();
    }

    @Nonnull
    @Override
    public Query<EntityStore> getQuery() {
        return StandardPhysicsProvider.getComponentType();
    }

    @Override
    public void onComponentAdded(@Nonnull Ref<EntityStore> ref, @Nonnull StandardPhysicsProvider physics,
                                 @Nonnull Store<EntityStore> store,
                                 @Nonnull CommandBuffer<EntityStore> commandBuffer) {
        weapons.record(ref, physics.getCreatorUuid(), store, commandBuffer);
    }

    @Override
    public void onComponentSet(@Nonnull Ref<EntityStore> ref, StandardPhysicsProvider oldComponent,
                               @Nonnull StandardPhysicsProvider newComponent, @Nonnull Store<EntityStore> store,
                               @Nonnull CommandBuffer<EntityStore> commandBuffer) {}

    @Override
    public void onComponentRemoved(@Nonnull Ref<EntityStore> ref, @Nonnull StandardPhysicsProvider component,
                                   @Nonnull Store<EntityStore> store,
                                   @Nonnull CommandBuffer<EntityStore> commandBuffer) {}
}
