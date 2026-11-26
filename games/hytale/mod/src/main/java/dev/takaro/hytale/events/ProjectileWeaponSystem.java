package dev.takaro.hytale.events;

import com.hypixel.hytale.component.CommandBuffer;
import com.hypixel.hytale.component.ComponentType;
import com.hypixel.hytale.component.Ref;
import com.hypixel.hytale.component.Store;
import com.hypixel.hytale.component.query.Query;
import com.hypixel.hytale.component.system.RefChangeSystem;
import com.hypixel.hytale.server.core.entity.UUIDComponent;
import com.hypixel.hytale.server.core.entity.entities.Player;
import com.hypixel.hytale.server.core.entity.entities.ProjectileComponent;
import com.hypixel.hytale.server.core.inventory.InventoryComponent;
import com.hypixel.hytale.server.core.inventory.ItemStack;
import com.hypixel.hytale.server.core.modules.entity.damage.Damage;
import com.hypixel.hytale.server.core.universe.world.storage.EntityStore;
import dev.takaro.hytale.TakaroPlugin;

import javax.annotation.Nonnull;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;

/** Records the firing item before the shooter can switch hotbar slots. */
public class ProjectileWeaponSystem extends RefChangeSystem<EntityStore, ProjectileComponent> {
    private static final long RETENTION_NANOS = java.util.concurrent.TimeUnit.MINUTES.toNanos(2);
    private final Map<UUID, Shot> shots = new ConcurrentHashMap<>();
    private final Map<Ref<EntityStore>, Shot> shotsByRef = new ConcurrentHashMap<>();
    private final TakaroPlugin plugin;

    public ProjectileWeaponSystem(TakaroPlugin plugin) {
        this.plugin = plugin;
    }

    private record Shot(String itemId, long createdAt) {}

    @Nonnull
    @Override
    public ComponentType<EntityStore, ProjectileComponent> componentType() {
        return ProjectileComponent.getComponentType();
    }

    @Nonnull
    @Override
    public Query<EntityStore> getQuery() {
        return ProjectileComponent.getComponentType();
    }

    @Override
    public void onComponentAdded(@Nonnull Ref<EntityStore> ref, @Nonnull ProjectileComponent projectile,
                                 @Nonnull Store<EntityStore> store,
                                 @Nonnull CommandBuffer<EntityStore> commandBuffer) {
        record(ref, projectile.getCreatorUuid(), store, commandBuffer);
    }

    void record(Ref<EntityStore> ref, UUID creatorUuid, Store<EntityStore> store,
                CommandBuffer<EntityStore> commandBuffer) {
        if (creatorUuid == null) {
            return;
        }
        Ref<EntityStore> shooter = ((EntityStore) store.getExternalData()).getRefFromUUID(creatorUuid);
        if (shooter == null || !shooter.isValid()
                || commandBuffer.getComponent(shooter, Player.getComponentType()) == null) {
            return;
        }
        ItemStack held = InventoryComponent.getItemInHand(commandBuffer, shooter);
        if (ItemStack.isEmpty(held)) {
            return;
        }
        long now = System.nanoTime();
        Shot shot = new Shot(held.getItemId(), now);
        shotsByRef.put(ref, shot);
        UUIDComponent projectileUuid = commandBuffer.getComponent(ref, UUIDComponent.getComponentType());
        if (projectileUuid != null) {
            shots.put(projectileUuid.getUuid(), shot);
        }
        // Projectiles despawn after 60 seconds. Retain snapshots briefly after removal because
        // death callbacks can run after the projectile was removed in the same tick.
        if (shotsByRef.size() > 256) {
            shotsByRef.entrySet().removeIf(entry -> now - entry.getValue().createdAt() > RETENTION_NANOS);
            shots.entrySet().removeIf(entry -> now - entry.getValue().createdAt() > RETENTION_NANOS);
        }
    }

    String weaponFor(Damage.ProjectileSource source, CommandBuffer<EntityStore> commandBuffer) {
        Ref<EntityStore> projectile = source.getProjectile();
        if (projectile == null) {
            return "";
        }
        Shot shot = shotsByRef.get(projectile);
        if (shot != null && System.nanoTime() - shot.createdAt() <= RETENTION_NANOS) {
            return shot.itemId();
        }
        if (!projectile.isValid()) {
            return "";
        }
        UUIDComponent id = commandBuffer.getComponent(projectile, UUIDComponent.getComponentType());
        if (id == null) {
            return "";
        }
        shot = shots.get(id.getUuid());
        return shot == null || System.nanoTime() - shot.createdAt() > RETENTION_NANOS
            ? "" : shot.itemId();
    }

    @Override
    public void onComponentSet(@Nonnull Ref<EntityStore> ref, ProjectileComponent oldComponent,
                               @Nonnull ProjectileComponent newComponent, @Nonnull Store<EntityStore> store,
                               @Nonnull CommandBuffer<EntityStore> commandBuffer) {}

    @Override
    public void onComponentRemoved(@Nonnull Ref<EntityStore> ref, @Nonnull ProjectileComponent component,
                                   @Nonnull Store<EntityStore> store,
                                   @Nonnull CommandBuffer<EntityStore> commandBuffer) {}
}
