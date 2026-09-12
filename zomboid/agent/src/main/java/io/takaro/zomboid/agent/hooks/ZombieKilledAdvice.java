package io.takaro.zomboid.agent.hooks;

import net.bytebuddy.asm.Advice;

/**
 * Inlined into
 * {@code zombie.characters.IsoZombie.onKilled(IsoGameCharacter killer, HandWeapon weapon, boolean gory)}.
 * {@code this} is the zombie that died; arg0 is the killer (only a player kill is
 * reported); arg1 is the weapon used. Decoding + rate-limiting happen in
 * {@link Bridge#zombieKilled}.
 */
public final class ZombieKilledAdvice {

    private ZombieKilledAdvice() {
    }

    @Advice.OnMethodEnter(suppress = Throwable.class)
    public static void enter(@Advice.Argument(0) Object killer, @Advice.Argument(1) Object weapon) {
        Bridge.zombieKilled(killer, weapon);
    }
}
