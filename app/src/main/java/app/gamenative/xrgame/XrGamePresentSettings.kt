package app.gamenative.xrgame

import com.winlator.core.envvars.EnvVars

/** Keep the UI and existing per-container opt-in on the same persisted setting. */
object XrGamePresentSettings {
    const val ASYNC_COPY_ENV = "XRGAME_PRESENT_ASYNC_COPY"
    const val FRAME_COMPLETION_ENV = "XRGAME_DXVK_FRAME_COMPLETION"

    fun asyncCopyEnabled(envVars: String): Boolean = EnvVars(envVars).get(ASYNC_COPY_ENV) == "1"

    fun withAsyncCopy(envVars: String, enabled: Boolean): String = EnvVars(envVars).apply {
        put(ASYNC_COPY_ENV, if (enabled) "1" else "0")
    }.toString()

    fun renderAheadEnabled(envVars: String): Boolean = EnvVars(envVars).get(FRAME_COMPLETION_ENV) == "gpu"

    fun withRenderAhead(envVars: String, enabled: Boolean): String = EnvVars(envVars).apply {
        put(FRAME_COMPLETION_ENV, if (enabled) "gpu" else "present")
    }.toString()
}
