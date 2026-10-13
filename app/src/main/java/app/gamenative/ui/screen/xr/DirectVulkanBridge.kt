package app.gamenative.ui.screen.xr

import com.winlator.renderer.VulkanXrFrameBridge
import timber.log.Timber

/** [VulkanXrFrameBridge] implementation for the default (dxvk/Vulkan) renderer path — unlike [DirectGLBridge], there's no buffer allocation or import to. */
class DirectVulkanBridge(
    private val onFrame: (ahbPtr: Long, width: Int, height: Int) -> Unit,
    private val onExchange: (exchangePtr: Long, width: Int, height: Int) -> Unit,
) : VulkanXrFrameBridge {
    private var announced = false

    override fun onScanoutBuffer(ahbPtr: Long, width: Int, height: Int) {
        announce("AHardwareBuffer", width, height)
        onFrame(ahbPtr, width, height)
    }

    override fun onSceneExchange(exchangePtr: Long, width: Int, height: Int) {
        announce("scene target ring", width, height)
        onExchange(exchangePtr, width, height)
    }

    private fun announce(what: String, width: Int, height: Int) {
        if (announced) return
        announced = true
        Timber.i(
            "Immersive: VulkanRenderer %s observed (%dx%d) — direct-render path active, PixelCopy of the game layer stopped",
            what,
            width,
            height,
        )
    }
}
