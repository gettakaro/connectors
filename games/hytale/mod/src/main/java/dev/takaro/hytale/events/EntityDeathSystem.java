package dev.takaro.hytale.events;

import com.hypixel.hytale.component.CommandBuffer;
import com.hypixel.hytale.component.ComponentType;
import com.hypixel.hytale.component.Ref;
import com.hypixel.hytale.component.Store;
import com.hypixel.hytale.component.query.Query;
import com.hypixel.hytale.component.system.RefChangeSystem;
import com.hypixel.hytale.server.core.entity.entities.Player;
import com.hypixel.hytale.server.core.entity.nameplate.Nameplate;
import com.hypixel.hytale.server.core.inventory.InventoryComponent;
import com.hypixel.hytale.server.core.inventory.ItemStack;
import com.hypixel.hytale.server.core.modules.entity.AllLegacyLivingEntityTypesQuery;
import com.hypixel.hytale.server.core.modules.entity.damage.Damage;
import com.hypixel.hytale.server.core.modules.entity.damage.DamageCause;
import com.hypixel.hytale.server.core.modules.entity.damage.DeathComponent;
import com.hypixel.hytale.server.core.universe.PlayerRef;
import com.hypixel.hytale.server.core.universe.world.storage.EntityStore;
import com.hypixel.hytale.server.npc.entities.NPCEntity;
import dev.takaro.hytale.TakaroPlugin;

import javax.annotation.Nonnull;
import java.util.HashMap;
import java.util.Map;

/**
 * Emits Takaro's {@code entity-killed} event when a player kills a non-player entity.
 *
 * <p>{@code DeathComponent} is generic to the entity store, not player-specific - the
 * connector's existing {@link PlayerDeathSystem} only saw players because its query was
 * {@code Player.getComponentType()}. This system watches all living entities instead, skips
 * players (they are {@link PlayerDeathSystem}'s job) and reports the kill only when the damage
 * source was a player.
 *
 * <p>Attribution comes from {@code DeathComponent.getDeathInfo()} -&gt; {@code Damage.getSource()};
 * a {@code Damage.EntitySource} carries the attacker's {@code Ref}, from which the killer's
 * {@code PlayerRef} is read. This is the same path Hytale's own PlayerKilledPlayer system uses.
 *
 * <p>Hytale 0.6.8 does not store a weapon on the death damage. Direct physical
 * hits use the attacker's held item; projectile hits use the item captured when
 * that projectile was created. Other delayed damage is left empty when the
 * originating item cannot be determined.
 *
 * <p>Everything here runs on the world thread that owns the dying entity, inside the ECS
 * callback. It does only component reads and hands the event to the WebSocket layer, which
 * queues rather than blocks.
 */
public class EntityDeathSystem extends RefChangeSystem<EntityStore, DeathComponent> {
    private final TakaroPlugin plugin;
    private final ProjectileWeaponSystem projectileWeapons;

    public EntityDeathSystem(TakaroPlugin plugin, ProjectileWeaponSystem projectileWeapons) {
        this.plugin = plugin;
        this.projectileWeapons = projectileWeapons;
    }

    @Nonnull
    @Override
    public ComponentType<EntityStore, DeathComponent> componentType() {
        return DeathComponent.getComponentType();
    }

    @Nonnull
    @Override
    public Query<EntityStore> getQuery() {
        return AllLegacyLivingEntityTypesQuery.INSTANCE;
    }

    @Override
    public void onComponentAdded(
            @Nonnull Ref<EntityStore> ref,
            @Nonnull DeathComponent deathComponent,
            @Nonnull Store<EntityStore> store,
            @Nonnull CommandBuffer<EntityStore> commandBuffer
    ) {
        try {
            // Player deaths are PlayerDeathSystem's business.
            if (commandBuffer.getComponent(ref, Player.getComponentType()) != null) {
                return;
            }

            PlayerRef killer = findKillingPlayer(deathComponent, commandBuffer);
            if (killer == null) {
                // A mob dying of fall damage or to another mob is not an entity-killed event.
                return;
            }

            Map<String, Object> player = new HashMap<>();
            player.put("name", killer.getUsername());
            player.put("gameId", killer.getUuid().toString());
            player.put("platformId", "hytale:" + killer.getUuid());

            Map<String, Object> eventData = new HashMap<>();
            eventData.put("player", player);
            eventData.put("entity", entityName(ref, commandBuffer));
            eventData.put("weapon", weaponForKill(deathComponent, commandBuffer));

            plugin.sendGameEventToAll("entity-killed", eventData);
        } catch (Exception e) {
            plugin.getLogger().at(java.util.logging.Level.SEVERE).log(
                "Error handling entity death: " + e.getMessage());
        }
    }

    private String weaponForKill(DeathComponent deathComponent, CommandBuffer<EntityStore> commandBuffer) {
        Damage damage = deathComponent.getDeathInfo();
        if (damage != null && damage.getSource() instanceof Damage.ProjectileSource projectileSource) {
            String firedItem = projectileWeapons.weaponFor(projectileSource, commandBuffer);
            if (!firedItem.isEmpty()) {
                return firedItem;
            }
        }
        // Bow impacts in Hytale 0.6.8 can arrive as a plain EntitySource with cause
        // Projectile, with no projectile reference to join to a launch snapshot.
        // Credit the equipped bow only while it is still held at the killing blow.
        if (damage != null && damage.getCause() != null
                && "Projectile".equalsIgnoreCase(damage.getCause().getId())
                && damage.getSource() instanceof Damage.EntitySource source) {
            Ref<EntityStore> attacker = source.getRef();
            if (attacker != null && attacker.isValid()
                    && commandBuffer.getComponent(attacker, Player.getComponentType()) != null) {
                ItemStack held = InventoryComponent.getItemInHand(commandBuffer, attacker);
                if (!ItemStack.isEmpty(held) && held.getItemId() != null
                        && held.getItemId().toLowerCase(java.util.Locale.ROOT).contains("bow")) {
                    return held.getItemId();
                }
            }
        }
        return weaponForDirectHit(deathComponent, commandBuffer);
    }

    /** The player who dealt the killing blow, or null when the killer was not a player. */
    static PlayerRef findKillingPlayer(DeathComponent deathComponent, CommandBuffer<EntityStore> commandBuffer) {
        Damage damage = deathComponent.getDeathInfo();
        if (damage == null) {
            return null;
        }
        Damage.Source source = damage.getSource();
        if (!(source instanceof Damage.EntitySource)) {
            return null;
        }
        Ref<EntityStore> attacker = ((Damage.EntitySource) source).getRef();
        if (attacker == null || !attacker.isValid()) {
            return null;
        }
        return commandBuffer.getComponent(attacker, PlayerRef.getComponentType());
    }

    /** Item id held by the player for a direct physical killing blow, when available. */
    static String weaponForDirectHit(DeathComponent deathComponent, CommandBuffer<EntityStore> commandBuffer) {
        Damage damage = deathComponent.getDeathInfo();
        if (damage == null || !(damage.getSource() instanceof Damage.EntitySource)
                || damage.getSource() instanceof Damage.ProjectileSource) {
            return "";
        }
        DamageCause cause = damage.getCause();
        if (cause == null || !"Physical".equalsIgnoreCase(cause.getId())) {
            return "";
        }
        Ref<EntityStore> attacker = ((Damage.EntitySource) damage.getSource()).getRef();
        if (attacker == null || !attacker.isValid()) {
            return "";
        }
        if (commandBuffer.getComponent(attacker, Player.getComponentType()) == null) {
            return "";
        }
        ItemStack held = InventoryComponent.getItemInHand(commandBuffer, attacker);
        return ItemStack.isEmpty(held) ? "" : held.getItemId();
    }

    /** Best human-readable name for a dead entity. */
    private String entityName(Ref<EntityStore> ref, CommandBuffer<EntityStore> commandBuffer) {
        try {
            Nameplate nameplate = commandBuffer.getComponent(ref, Nameplate.getComponentType());
            if (nameplate != null && nameplate.getText() != null && !nameplate.getText().isEmpty()) {
                return nameplate.getText();
            }
        } catch (Exception ignored) {
            // fall through
        }
        try {
            NPCEntity npc = commandBuffer.getComponent(ref, NPCEntity.getComponentType());
            if (npc != null && npc.getRoleName() != null) {
                return npc.getRoleName();
            }
        } catch (Exception ignored) {
            // fall through
        }
        return "unknown";
    }

    @Override
    public void onComponentSet(
            @Nonnull Ref<EntityStore> ref,
            DeathComponent oldComponent,
            @Nonnull DeathComponent newComponent,
            @Nonnull Store<EntityStore> store,
            @Nonnull CommandBuffer<EntityStore> commandBuffer
    ) {
        // Only the initial death matters.
    }

    @Override
    public void onComponentRemoved(
            @Nonnull Ref<EntityStore> ref,
            @Nonnull DeathComponent component,
            @Nonnull Store<EntityStore> store,
            @Nonnull CommandBuffer<EntityStore> commandBuffer
    ) {
        // Only the initial death matters.
    }
}
