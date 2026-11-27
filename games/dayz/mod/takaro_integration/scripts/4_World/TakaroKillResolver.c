// Shared killer-resolution helpers for death / entity-killed events.
//
// Since BI T170455 (DayZ 1.20) the `killer` argument handed to EEKilled is
// frequently NOT the player: it is the weapon, the projectile source or the
// vehicle that dealt the final blow. Resolving the responsible player
// therefore needs three attempts, in order:
//   1. killer is itself a PlayerBase
//   2. killer is an EntityAI whose hierarchy root is a player (weapon in hand)
//   3. the EntityAI cached from the victim's last EEHitBy, same treatment
// Anything else (zombie mauling, fall damage, starvation) resolves to null,
// which is the correct "no player attacker" answer.
class TakaroKillerResolver
{
    static PlayerBase Resolve(Object killer, EntityAI cachedSource)
    {
        PlayerBase player;
        if (killer)
        {
            player = PlayerBase.Cast(killer);
            if (player) return player;

            EntityAI ek = EntityAI.Cast(killer);
            if (ek)
            {
                player = PlayerBase.Cast(ek.GetHierarchyRootPlayer());
                if (player) return player;
            }
        }

        if (cachedSource)
        {
            player = PlayerBase.Cast(cachedSource);
            if (player) return player;

            player = PlayerBase.Cast(cachedSource.GetHierarchyRootPlayer());
            if (player) return player;
        }

        return null;
    }

    // Weapon name for the event. When the killer object is not the player
    // itself it IS the weapon/projectile source, so its classname is the best
    // answer available in script. Otherwise fall back to the ammo classname
    // cached from EEHitBy (melee/unarmed report the player as killer).
    static string ResolveWeapon(Object killer, string cachedAmmo)
    {
        if (killer)
        {
            PlayerBase asPlayer = PlayerBase.Cast(killer);
            if (!asPlayer) return killer.GetType();
        }
        return cachedAmmo;
    }
}
