package app.gamenative.xrgame

import android.content.Context
import app.gamenative.R
import app.gamenative.data.GameSource
import app.gamenative.utils.launchdependencies.LaunchDependency
import app.gamenative.utils.launchdependencies.LaunchDependencyCallbacks
import com.winlator.container.Container

/** Revalidate the selected runtime before each launch, including cached installations. */
object XrGameRuntimeDependency : LaunchDependency {
    override fun appliesTo(container: Container, gameSource: GameSource, gameId: Int) = true
    override fun isSatisfied(context: Context, container: Container, gameSource: GameSource, gameId: Int) = false
    override fun getLoadingMessage(context: Context, container: Container, gameSource: GameSource, gameId: Int) =
        context.getString(R.string.main_loading)
    override suspend fun install(context: Context, container: Container, callbacks: LaunchDependencyCallbacks,
                                 gameSource: GameSource, gameId: Int) {
        XrGameRuntime.prepare(context, container, callbacks.setLoadingProgress)
    }
}
