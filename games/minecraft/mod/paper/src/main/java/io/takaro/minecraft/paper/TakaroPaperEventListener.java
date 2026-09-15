package io.takaro.minecraft.paper;

import io.takaro.minecraft.core.EventEmitter;
import io.papermc.paper.event.player.AsyncChatEvent;
import net.kyori.adventure.text.serializer.plain.PlainTextComponentSerializer;
import org.bukkit.entity.LivingEntity;
import org.bukkit.entity.Player;
import org.bukkit.event.EventHandler;
import org.bukkit.event.Listener;
import org.bukkit.event.entity.EntityDeathEvent;
import org.bukkit.event.entity.PlayerDeathEvent;
import org.bukkit.event.player.PlayerJoinEvent;
import org.bukkit.event.player.PlayerQuitEvent;

public class TakaroPaperEventListener implements Listener {

    private final TakaroPaperPlugin plugin;

    public TakaroPaperEventListener(TakaroPaperPlugin plugin) {
        this.plugin = plugin;
    }

    @EventHandler
    public void onPlayerJoin(PlayerJoinEvent event) {
        EventEmitter emitter = plugin.getEventEmitter();
        if (emitter == null) return;
        emitter.emitPlayerConnected(plugin.toPlayerInfo(event.getPlayer()));
    }

    @EventHandler
    public void onPlayerQuit(PlayerQuitEvent event) {
        EventEmitter emitter = plugin.getEventEmitter();
        if (emitter == null) return;
        Player player = event.getPlayer();
        String gameId = player.getUniqueId().toString();
        plugin.getPlayerLocation(gameId); // warm cache before player is removed from list
        emitter.emitPlayerDisconnected(gameId, player.getName());
    }

    @EventHandler
    public void onChat(AsyncChatEvent event) {
        EventEmitter emitter = plugin.getEventEmitter();
        if (emitter == null) return;
        Player player = event.getPlayer();
        String message = PlainTextComponentSerializer.plainText().serialize(event.message());
        emitter.emitChatMessage(
                player.getUniqueId().toString(),
                player.getName(),
                "global",
                message
        );
    }

    @EventHandler
    public void onPlayerDeath(PlayerDeathEvent event) {
        EventEmitter emitter = plugin.getEventEmitter();
        if (emitter == null) return;
        Player victim = event.getEntity();
        var loc = victim.getLocation();
        String dimension = plugin.mapDimension(loc.getWorld());

        Player killer = victim.getKiller();
        emitter.emitPlayerDeath(
                victim.getUniqueId().toString(),
                victim.getName(),
                killer != null ? killer.getUniqueId().toString() : null,
                killer != null ? killer.getName() : null,
                loc.getX(), loc.getY(), loc.getZ(),
                dimension
        );
    }

    @EventHandler
    public void onEntityDeath(EntityDeathEvent event) {
        EventEmitter emitter = plugin.getEventEmitter();
        if (emitter == null) return;
        LivingEntity entity = event.getEntity();
        if (entity instanceof Player) return;
        Player killer = entity.getKiller();
        if (killer == null) return;

        String weaponCode = "";
        var mainHand = killer.getInventory().getItemInMainHand();
        if (mainHand.getType() != org.bukkit.Material.AIR) {
            weaponCode = mainHand.getType().getKey().toString();
        }

        emitter.emitEntityKilled(
                killer.getUniqueId().toString(),
                killer.getName(),
                entity.getType().getKey().toString(),
                weaponCode
        );
    }
}
