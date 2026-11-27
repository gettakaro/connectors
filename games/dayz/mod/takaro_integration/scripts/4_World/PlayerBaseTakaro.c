// Hook player death so we can emit the canonical Takaro `player-death` event.
// Most kill data comes from EEKilled — we extract the killer entity and
// best-guess the weapon classname.
//
// F4b: since BI T170455 the `killer` argument is frequently the weapon or the
// vehicle rather than the shooter, so the attacker is resolved through
// TakaroKillerResolver (killer -> hierarchy root player). Infected/animal and
// environmental deaths stay attacker-less; the killer classname still lands in
// the event `msg` via TakaroEventFactory.Death.
modded class PlayerBase
{
    EntityAI m_TakaroLastSource;
    string m_TakaroLastAmmo;
    int m_TakaroLastHitTime;

    override void EEHitBy(TotalDamageResult damageResult, int damageType, EntityAI source, int component, string dmgZone, string ammo, vector modelPos, float speedCoef)
    {
        super.EEHitBy(damageResult, damageType, source, component, dmgZone, ammo, modelPos, speedCoef);

        if (source)
        {
            m_TakaroLastSource = source;
            m_TakaroLastAmmo = ammo;
            m_TakaroLastHitTime = GetGame().GetTime();
        }
    }

    override void EEKilled(Object killer)
    {
        super.EEKilled(killer);

        TakaroBridge bridge = TakaroBridge.Cast(TakaroBridgeAccessor.Get());
        if (!bridge) return;

        string weaponName = "";
        if (killer)
            weaponName = killer.GetType();

        PlayerBase attacker = TakaroKillerResolver.Resolve(killer, m_TakaroLastSource);
        // M3 fix: bleed-out / fall / environmental deaths hand the victim itself
        // in as `killer`, which produced attacker == victim and
        // "killed with SurvivorF_Linda". A self-kill has no attacker; name the
        // last damage source instead (e.g. the infected that caused the bleed).
        if (attacker == this) attacker = null;
        if (killer == this)
        {
            weaponName = "";
            if (m_TakaroLastSource && m_TakaroLastSource != this)
                weaponName = m_TakaroLastSource.GetType();
            else if (m_TakaroLastAmmo != "")
                weaponName = m_TakaroLastAmmo;
        }

        EntityAI killerEntity;
        if (attacker)
            killerEntity = EntityAI.Cast(attacker);
        else if (killer && killer != this)
            killerEntity = EntityAI.Cast(killer);

        bridge.OnPlayerKilled(this, killerEntity, weaponName);
    }
}
