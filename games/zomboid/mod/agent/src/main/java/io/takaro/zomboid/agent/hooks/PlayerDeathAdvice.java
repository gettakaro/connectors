package io.takaro.zomboid.agent.hooks;

import net.bytebuddy.asm.Advice;

/**
 * Inlined into
 * {@code zombie.characters.IsoPlayer.onKilled(IsoGameCharacter, HandWeapon, boolean)}.
 * {@code this} is the victim; arg0 is the killer (an IsoPlayer when PvP).
 */
public final class PlayerDeathAdvice {

    private PlayerDeathAdvice() {
    }

    @Advice.OnMethodEnter(suppress = Throwable.class)
    public static void enter(@Advice.This Object victim, @Advice.Argument(0) Object killer) {
        Bridge.playerDeath(victim, killer);
    }
}
