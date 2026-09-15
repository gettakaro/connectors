package io.takaro.zomboid.agent;

import io.takaro.zomboid.agent.hooks.Bridge;

import zombie.network.server.IEventController;

/**
 * A text-only server-report sink registered on
 * {@code zombie.network.server.EventManager} (the same channel Discord/StackBot
 * integrations use). Every {@link #process(String)} line is forwarded to
 * {@link Bridge#logLine(String)}, which rate-limits and (only when
 * {@code logEvents=true}) emits a Takaro {@code log} event.
 */
public final class EventManagerCallback implements IEventController {

    @Override
    public void process(String message) {
        Bridge.logLine(message);
    }
}
