package app.gamenative.xrgame

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.withTimeoutOrNull

/** Bounds a CM preparation request and its children without treating user cancellation as failure. */
internal object XrGameSteamRequest {
    class TimedOut(val stage: String) : Exception("Steam content request timed out: $stage")
    private data class Result<T>(val value: T)

    suspend fun <T> run(stage: String, timeoutMs: Long = 30_000L, block: suspend CoroutineScope.() -> T): T {
        // Box nullable results. withTimeoutOrNull only handles its own deadline;
        // parent cancellation and a nested timeout retain their original meaning.
        return (withTimeoutOrNull(timeoutMs) { Result(block()) } ?: throw TimedOut(stage)).value
    }
}
