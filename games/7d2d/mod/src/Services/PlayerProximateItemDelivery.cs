using UnityEngine;

namespace Takaro.Services
{
    /// <summary>
    /// Delivers one stack through the game's normal replicated world-item path
    /// at the target player's vanilla drop position, matching the first-party
    /// remote give command without assigning an owning entity.
    /// </summary>
    public static class PlayerProximateItemDelivery
    {
        public static void Drop(ItemValue itemValue, int amount, EntityPlayer player)
        {
            var itemStack = new ItemStack(itemValue, amount);
            GameManager.Instance.ItemDropServer(itemStack, player.GetDropPosition(), Vector3.zero);
        }
    }
}
