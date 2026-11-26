package dev.takaro.hytale.util;

import org.junit.jupiter.api.Test;

import java.util.logging.Level;

import static org.junit.jupiter.api.Assertions.*;

class LogForwardFilterTest {

    @Test
    void theWireLoggerIsNeverForwarded() {
        // This is the whole defence against the log -> gameEvent -> log amplification loop.
        LogForwardFilter f = new LogForwardFilter(Level.FINEST, 0);
        assertFalse(f.shouldForward(LogForwardFilter.WIRE_LOGGER_NAME, Level.INFO, 0));
        assertFalse(f.shouldForward(LogForwardFilter.WIRE_LOGGER_NAME, Level.SEVERE, 0));
        assertFalse(f.shouldForward(LogForwardFilter.WIRE_LOGGER_NAME + ".frames", Level.INFO, 0));
        assertTrue(LogForwardFilter.isExcludedLogger("takaro.wire"));
        assertFalse(LogForwardFilter.isExcludedLogger("dev.takaro.hytale.TakaroPlugin"));
        assertFalse(LogForwardFilter.isExcludedLogger(null));
    }

    @Test
    void ordinaryLoggersAreForwarded() {
        LogForwardFilter f = new LogForwardFilter(Level.INFO, 0);
        assertTrue(f.shouldForward("com.hypixel.hytale.Server", Level.INFO, 0));
        assertTrue(f.shouldForward(null, Level.WARNING, 0));
    }

    @Test
    void recordsBelowTheConfiguredLevelAreDropped() {
        LogForwardFilter f = new LogForwardFilter(Level.WARNING, 0);
        assertFalse(f.shouldForward("x", Level.INFO, 0));
        assertFalse(f.shouldForward("x", Level.FINE, 0));
        assertTrue(f.shouldForward("x", Level.WARNING, 0));
        assertTrue(f.shouldForward("x", Level.SEVERE, 0));
    }

    @Test
    void theRateCapAppliesPerMinuteAndCountsSuppression() {
        LogForwardFilter f = new LogForwardFilter(Level.INFO, 3);
        for (int i = 0; i < 3; i++) {
            assertTrue(f.shouldForward("x", Level.INFO, 1_000));
        }
        assertFalse(f.shouldForward("x", Level.INFO, 1_000));
        assertFalse(f.shouldForward("x", Level.INFO, 30_000));
        assertEquals(2, f.suppressedCount());

        // New window
        assertTrue(f.shouldForward("x", Level.INFO, 70_000));
    }

    @Test
    void zeroOrNegativeCapMeansUnlimited() {
        LogForwardFilter f = new LogForwardFilter(Level.INFO, 0);
        for (int i = 0; i < 10_000; i++) {
            assertTrue(f.shouldForward("x", Level.INFO, 0));
        }
        assertEquals(0, f.suppressedCount());
    }

    @Test
    void levelParsingFallsBackInsteadOfThrowing() {
        assertEquals(Level.WARNING, LogForwardFilter.parseLevel("warning", Level.INFO));
        assertEquals(Level.OFF, LogForwardFilter.parseLevel("OFF", Level.INFO));
        assertEquals(Level.INFO, LogForwardFilter.parseLevel("nonsense", Level.INFO));
        assertEquals(Level.INFO, LogForwardFilter.parseLevel(null, Level.INFO));
        assertEquals(Level.INFO, LogForwardFilter.parseLevel("  ", Level.INFO));
    }
}
