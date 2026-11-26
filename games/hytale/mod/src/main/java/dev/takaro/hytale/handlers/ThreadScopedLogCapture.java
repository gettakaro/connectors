package dev.takaro.hytale.handlers;

import com.hypixel.hytale.logger.backend.HytaleLoggerBackend;

import dev.takaro.hytale.util.CommandOutput;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.logging.LogRecord;

/**
 * Collects log records emitted <em>by the thread(s) executing one Takaro console command</em>,
 * for the second half of the hybrid capture.
 *
 * <p>{@code HytaleLoggerBackend.subscribe(CopyOnWriteArrayList&lt;LogRecord&gt;)} is a
 * <b>global</b> tap: the backend appends every record from every thread to every subscribed
 * list. Subscribing to it and returning the lot is precisely how other output leaked in. This class
 * subclasses the list the backend expects and overrides {@link #add(Object)} so that a record
 * is only kept when {@link LogRecord#getLongThreadID()} matches a thread that is known to be
 * running this command. Everything else is dropped in the caller's frame - nothing is ever
 * stored in the underlying list, so there is no copy-on-write churn and no memory growth from
 * foreign traffic.
 *
 * <p>Thread attribution is possible because
 * {@code CommandManager.handleCommand(sender, cmd)} hands the work to
 * {@code ForkJoinPool.commonPool()} and the task then calls {@code CommandSender.getUsername()}
 * (for its own {@code "%s executed command: %s"} log line) <b>before</b> parsing or running the
 * command. {@link OutputCapturingCommandSender} records {@code Thread.currentThread().threadId()}
 * on every {@code CommandSender} callback, so the executing thread is registered here before the
 * command can produce any output - and a command that later hops to another thread registers
 * that thread too as soon as it writes to the sender.
 *
 * <p>Known limit: a command that hops to a world thread and logs there <em>without</em> ever
 * touching its sender from that thread cannot be attributed, and its log lines are left out
 * rather than guessed at.
 */
public final class ThreadScopedLogCapture extends CopyOnWriteArrayList<LogRecord> {
    private static final long serialVersionUID = 1L;

    private final Set<Long> threadIds = ConcurrentHashMap.newKeySet();
    private final List<String> lines = Collections.synchronizedList(new ArrayList<>());
    private volatile boolean open = true;

    /** Register a thread that is executing this command. */
    public void markCurrentThread() {
        threadIds.add(Thread.currentThread().threadId());
    }

    /** True once at least one executing thread is known. */
    public boolean hasThreads() {
        return !threadIds.isEmpty();
    }

    @Override
    public boolean add(LogRecord record) {
        if (!open || record == null) {
            return false;
        }
        if (!threadIds.contains(record.getLongThreadID())) {
            return false;
        }
        if (CommandOutput.isExcludedLogger(record.getLoggerName())) {
            return false;
        }
        String message = record.getMessage();
        if (message == null || message.isBlank()) {
            return false;
        }
        if (lines.size() >= CommandOutput.MAX_LINES) {
            return false;
        }
        lines.add(message);
        // Never store the record itself: the backend ignores the return value and we do not
        // want the CopyOnWriteArrayList to grow.
        return false;
    }

    /** The captured lines, in emission order. */
    public List<String> getLines() {
        synchronized (lines) {
            return new ArrayList<>(lines);
        }
    }

    /** Subscribe to the server logger. Always paired with {@link #close()} in a finally block. */
    public void subscribe() {
        HytaleLoggerBackend.subscribe(this);
    }

    /** Stop collecting and unsubscribe. Idempotent. */
    public void close() {
        open = false;
        try {
            HytaleLoggerBackend.unsubscribe(this);
        } catch (RuntimeException ignored) {
            // Never let teardown break a command response.
        }
    }
}
