package app.gamenative.xrgame

import com.winlator.core.envvars.EnvVars

/** Keep the UI and existing per-container opt-in on the same persisted setting. */
object XrGamePresentSettings {
    const val ASYNC_COPY_ENV = "XRGAME_PRESENT_ASYNC_COPY"

    fun asyncCopyEnabled(envVars: String): Boolean = EnvVars(envVars).get(ASYNC_COPY_ENV) == "1"

    fun withAsyncCopy(envVars: String, enabled: Boolean): String = EnvVars(envVars).apply {
        put(ASYNC_COPY_ENV, if (enabled) "1" else "0")
    }.toString()
}
