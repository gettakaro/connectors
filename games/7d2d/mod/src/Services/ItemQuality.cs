namespace Takaro.Services
{
    /// <summary>
    /// Decides the quality giveItem applies. Takaro sends a quality with every listing,
    /// also for items that have none (ammo, resources), so a quality is only checked for
    /// items that use one.
    /// </summary>
    public static class ItemQuality
    {
        /// <summary>
        /// False with an error message when the item uses quality and
        /// <paramref name="requested"/> is not a number from 0 to <paramref name="max"/>.
        /// Without a request the item gets <paramref name="max"/>; an item without quality
        /// ignores the request.
        /// </summary>
        public static bool TryResolve(
            string requested,
            bool itemUsesQuality,
            ushort max,
            out ushort quality,
            out string error
        )
        {
            quality = max;
            error = null;
            if (!itemUsesQuality || string.IsNullOrEmpty(requested))
                return true;

            if (ushort.TryParse(requested, out ushort parsed) && parsed <= max)
            {
                quality = parsed;
                return true;
            }

            error = $"Invalid quality value '{requested}': expected 0 to {max}";
            return false;
        }
    }
}
