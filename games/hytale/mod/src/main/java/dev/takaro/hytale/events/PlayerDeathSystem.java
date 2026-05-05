package dev.takaro.hytale.events;

import com.hypixel.hytale.component.CommandBuffer;
import com.hypixel.hytale.component.ComponentType;
import com.hypixel.hytale.component.Ref;
import com.hypixel.hytale.component.Store;
import com.hypixel.hytale.component.query.Query;
import com.hypixel.hytale.component.system.RefChangeSystem;
import com.hypixel.hytale.server.core.entity.entities.Player;
import com.hypixel.hytale.server.core.modules.entity.component.TransformComponent;
import com.hypixel.hytale.server.core.modules.entity.damage.Damage;
import com.hypixel.hytale.server.core.modules.entity.damage.DeathComponent;
import com.hypixel.hytale.server.core.universe.PlayerRef;
import com.hypixel.hytale.server.core.universe.world.storage.EntityStore;
import dev.takaro.hytale.TakaroPlugin;

import javax.annotation.Nonnull;
import java.util.HashMap;
import java.util.Map;

/**
 * ECS system that detects when players die and forwards the event to Takaro
 * This extends RefChangeSystem to monitor when DeathComponent is added to Player entities
 */
public class PlayerDeathSystem extends RefChangeSystem<EntityStore, DeathComponent> {
    private final TakaroPlugin plugin;

    public PlayerDeathSystem(TakaroPlugin plugin) {
        this.plugin = plugin;
    }

    @Nonnull
    @Override
    public ComponentType<EntityStore, DeathComponent> componentType() {
        return DeathComponent.getComponentType();
    }

    @Nonnull
    @Override
    public Query<EntityStore> getQuery() {
        // Only watch for death on Player entities
        return Player.getComponentType();
    }

    @Override
    public void onComponentAdded(
            @Nonnull Ref<EntityStore> ref,
            @Nonnull DeathComponent deathComponent,
            @Nonnull Store<EntityStore> store,
            @Nonnull CommandBuffer<EntityStore> commandBuffer
    ) {
        try {
            // Get the Player component
            Player playerComponent = commandBuffer.getComponent(ref, Player.getComponentType());
            if (playerComponent == null) {
                return;
            }

            // Get the PlayerRef component to access player identity
            PlayerRef playerRef = commandBuffer.getComponent(ref, PlayerRef.getComponentType());
            if (playerRef == null) {
                return;
            }

            String playerName = playerRef.getUsername();
            String uuid = playerRef.getUuid().toString();

            plugin.getLogger().at(java.util.logging.Level.FINE).log("[EVENT] Player died: " + playerName);

            // Get player's position at time of death
            TransformComponent transform = commandBuffer.getComponent(ref, TransformComponent.getComponentType());

            // Build death event for Takaro
            Map<String, Object> player = new HashMap<>();
            player.put("name", playerName);
            player.put("gameId", uuid);
            player.put("platformId", "hytale:" + uuid);

            Map<String, Object> eventData = new HashMap<>();
            eventData.put("type", "player-death");
            eventData.put("player", player);

            // Add position if available
            if (transform != null) {
                Map<String, Object> position = new HashMap<>();
                position.put("x", transform.getPosition().getX());
                position.put("y", transform.getPosition().getY());
                position.put("z", transform.getPosition().getZ());
                eventData.put("position", position);
            }

            // Add death cause as message if available
            Damage deathInfo = deathComponent.getDeathInfo();
            if (deathInfo != null && deathComponent.getDeathCause() != null) {
                eventData.put("msg", playerName + " died: " + deathComponent.getDeathCause().getId());
            } else {
                eventData.put("msg", playerName + " died");
            }

            // Send to all Takaro connections (production and dev if enabled)
            plugin.sendGameEventToAll("player-death", eventData);

        } catch (Exception e) {
            plugin.getLogger().at(java.util.logging.Level.SEVERE).log("Error handling player death: " + e.getMessage());
            e.printStackTrace();
        }
    }

    @Override
    public void onComponentSet(
            @Nonnull Ref<EntityStore> ref,
            DeathComponent oldComponent,
            @Nonnull DeathComponent newComponent,
            @Nonnull Store<EntityStore> store,
            @Nonnull CommandBuffer<EntityStore> commandBuffer
    ) {
        // Not needed - we only care about initial death
    }

    @Override
    public void onComponentRemoved(
            @Nonnull Ref<EntityStore> ref,
            @Nonnull DeathComponent component,
            @Nonnull Store<EntityStore> store,
            @Nonnull CommandBuffer<EntityStore> commandBuffer
    ) {
        // Not needed - we only care about death, not respawn
    }
}
