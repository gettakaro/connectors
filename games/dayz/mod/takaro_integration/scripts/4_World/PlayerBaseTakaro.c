// Hook player death so we can emit the canonical Takaro `player-death` event.
// Most kill data comes from EEKilled — we extract the killer entity and
// best-guess the weapon classname.

modded class PlayerBase
{
    override void EEKilled(Object killer)
    {
        super.EEKilled(killer);

        TakaroBridge bridge = TakaroBridge.Cast(TakaroBridgeAccessor.Get());
        if (!bridge) return;

        EntityAI killerEntity;
        string weaponName = "";

        if (killer)
        {
            killerEntity = EntityAI.Cast(killer);
            weaponName = killer.GetType();
        }

        bridge.OnPlayerKilled(this, killerEntity, weaponName);
    }
}
