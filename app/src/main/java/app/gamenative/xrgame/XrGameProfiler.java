package app.gamenative.xrgame;

import android.content.Context;
import app.gamenative.BuildConfig;

/** Optional internal diagnostics. Regions may cross coroutine threads; they measure elapsed time. */
public final class XrGameProfiler {
    public interface Region extends AutoCloseable { @Override void close(); }
    public interface Backend {
        Region region(String name);
        void mark(String name);
        void present();
        default void counter(int id, long value) {}
    }
    private static final Region NO_REGION = () -> {};
    private static volatile Backend backend;
    private XrGameProfiler() {}

    public static Region noop() { return NO_REGION; }
    public static void counter(int id, long sample) {
        Backend value = backend;
        if (value != null) value.counter(id, sample);
    }

    public static void initialize(Context context) {
        if (!BuildConfig.XRGAME || !BuildConfig.DEBUG) return;
        try {
            backend = (Backend) Class.forName("app.gamenative.xrgame.LitepProfiler")
                    .getConstructor(Context.class).newInstance(context.getApplicationContext());
        } catch (ReflectiveOperationException | LinkageError e) {
            android.util.Log.w("XRGameProfiler", "Internal profiler unavailable", e);
        }
    }
    public static Region region(String name) {
        Backend value = backend;
        return value == null ? NO_REGION : value.region(name);
    }
    public static void mark(String name) {
        Backend value = backend;
        if (value != null) value.mark(name);
    }
    /** One real X11 Present request, not a displayed Android frame or a synthetic timer. */
    public static void present() {
        Backend value = backend;
        if (value != null) value.present();
    }
}
