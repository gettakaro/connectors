package io.takaro.minecraft.core;

import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;

/** Test adapters shared across packages. */
public final class TakaroConnectorTestSupport {
    private TakaroConnectorTestSupport() {}

    /** Records log lines; every game method is a no-op. */
    public static class LogAdapter extends TakaroConnectorTest.TestAdapter {
        public final List<String> infos = new CopyOnWriteArrayList<>();
        public final List<String> warnings = new CopyOnWriteArrayList<>();
        public volatile EventEmitter emitter;

        @Override
        public void logInfo(String msg) { infos.add(msg); }

        @Override
        public void logWarning(String msg) { warnings.add(msg); }

        @Override
        public void setEventEmitter(EventEmitter emitter) { this.emitter = emitter; }
    }
}
