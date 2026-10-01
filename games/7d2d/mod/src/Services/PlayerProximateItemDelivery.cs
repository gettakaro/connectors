using UnityEngine;

namespace Takaro.Services
{
    /// <summary>
    /// Puts a given stack straight into the player's inventory.
    ///
    /// 7D2D inventories are client-authoritative: a server-side Bag edit is
    /// overwritten by the next client sync. The way to add an item is the one
    /// the game itself uses for a pickup: spawn the item entity at the player,
    /// owned by them, tell their client it collected that entity
    /// (NetPackageEntityCollect), and remove the entity. The client then adds
    /// the stack to its own inventory exactly as if the player had pressed E.
    /// Same approach as ServerCore's `giveplus`. Earlier versions only did the
    /// first step, leaving a bag on the ground for the player to pick up.
    ///
    /// Without a connected client to tell (should not happen for a spawned
    /// player) the stack is dropped at the player's feet instead.
    /// </summary>
    public static class PlayerProximateItemDelivery
    {
        public const float ItemLifetimeSeconds = 60f;

        public static bool Deliver(ItemValue itemValue, int amount, EntityPlayer player, ClientInfo client)
        {
            var itemStack = new ItemStack(itemValue, amount);
            if (client == null)
            {
                Drop(itemStack, player);
                return false;
            }

            World world = GameManager.Instance.World;
            var entityItem = (EntityItem)EntityFactory.CreateEntity(
                new EntityCreationData
                {
                    entityClass = EntityClass.FromString("item"),
                    id = EntityFactory.nextEntityID++,
                    itemStack = itemStack,
                    pos = player.GetDropPosition(),
                    rot = new Vector3(20f, 0f, 20f),
                    lifetime = ItemLifetimeSeconds,
                    belongsPlayerId = player.entityId,
                }
            );
            world.SpawnEntityInWorld(entityItem);
            client.SendPackage(
                NetPackageManager
                    .GetPackage<NetPackageEntityCollect>()
                    .Setup(entityItem.entityId, player.entityId)
            );
            world.RemoveEntity(entityItem.entityId, EnumRemoveEntityReason.Killed);
            return true;
        }

        private static void Drop(ItemStack itemStack, EntityPlayer player)
        {
            GameManager.Instance.ItemDropServer(itemStack, player.GetDropPosition(), Vector3.zero);
        }
    }
}
