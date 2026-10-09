package app.gamenative.ui.screen.xr.windows

import app.gamenative.BuildConfig
import com.winlator.container.Container

data class WindowsVrRuntimeConfig(
    val enabled: Boolean,
    val openCompositeEnabled: Boolean,
    val controlPort: Int = 38476,
    val protocolVersion: Int = 2,
    val runtimeDirectory: String = "C:\\gamenative-xr",
    val runtimeManifest: String = "C:\\gamenative-xr\\active_runtime.json",
    val transportEndpoint: String = "@gamenative-xr",
    val renderScalePercent: Int = 100,
    val renderScaleRecommended: Boolean = false,
) {
    companion object {
        fun from(container: Container): WindowsVrRuntimeConfig {
            val recommended = BuildConfig.XRGAME && container.getExtra(
                XrResolutionRecommendation.EXTRA_MODE, XrResolutionRecommendation.MODE_RECOMMENDED,
            ) == XrResolutionRecommendation.MODE_RECOMMENDED
            return WindowsVrRuntimeConfig(
                enabled = container.getExtra("windowsVrEnabled", "false").toBoolean(),
                openCompositeEnabled = container.getExtra("windowsVrOpenCompositeEnabled", "false").toBoolean(),
                renderScalePercent = if (recommended) XrResolutionRecommendation.RECOMMENDED_PERCENT
                else container.xrRenderScale.coerceIn(25, 100),
                renderScaleRecommended = recommended,
            )
        }
    }
}
