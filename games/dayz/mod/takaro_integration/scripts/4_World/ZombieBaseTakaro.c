// F4 — emit Takaro `entity-killed` when a player kills an infected.
//
// EEKilled's `killer` argument is unreliable (BI T170455), so we cache the
// EEHitBy source/ammo on every hit and use it as the fallback when resolving
// the responsible player.
modded class ZombieBase
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

        PlayerBase player = TakaroKillerResolver.Resolve(killer, m_TakaroLastSource);
        string killerType = "null";
        if (killer) killerType = killer.GetType();
        string srcType = "null";
        if (m_TakaroLastSource) srcType = m_TakaroLastSource.GetType();
        TakaroLog.Debug("ZombieBase.EEKilled " + GetType() + " killer=" + killerType + " lastSource=" + srcType + " ammo=" + m_TakaroLastAmmo);
        if (!player) return;   // environment / other-AI kill: not a Takaro event

        string weapon = TakaroKillerResolver.ResolveWeapon(killer, m_TakaroLastAmmo);
        bridge.OnEntityKilled(player, this.GetType(), weapon, this.GetPosition());
    }
}
