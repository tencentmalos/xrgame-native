package app.gamenative.ui.screen.xr

import androidx.compose.runtime.saveable.SaverScope
import com.winlator.container.Container
import com.winlator.container.ContainerData
import org.junit.Assert.*
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [33], manifest = Config.NONE)
class SbsTheaterSettingsTest {
    private fun container() = Container("test").apply { graphicsDriver = "wrapper" }

    @Test fun `existing flat and VR containers stay out of cinema`() {
        assumeTrue(app.gamenative.BuildConfig.XRGAME)
        val c = container()
        assertFalse(SbsTheaterSettings.isEnabled(c))
        c.putExtra(SbsTheaterSettings.ENABLED, "true")
        assertTrue(SbsTheaterSettings.isEnabled(c))
        c.putExtra("windowsVrEnabled", "true")
        assertFalse(SbsTheaterSettings.isEnabled(c))
        c.putExtra("windowsVrEnabled", "false")
        c.graphicsDriver = "virgl"
        assertFalse(SbsTheaterSettings.isEnabled(c))
    }

    @Test fun `headset quad settings are reused and invalid values bounded`() {
        val c = container()
        assertEquals(1f, SbsTheaterSettings.scale(c), 0f)
        assertEquals(2f, SbsTheaterSettings.distance(c), 0f)
        c.putExtra("immersiveQuadScale", "1.75")
        c.putExtra("immersiveQuadDistance", "3.5")
        assertEquals(1.75f, SbsTheaterSettings.scale(c), 0f)
        assertEquals(3.5f, SbsTheaterSettings.distance(c), 0f)
        c.putExtra("immersiveQuadScale", "NaN")
        c.putExtra("immersiveQuadDistance", "Infinity")
        assertEquals(1f, SbsTheaterSettings.scale(c), 0f)
        assertEquals(2f, SbsTheaterSettings.distance(c), 0f)
        c.putExtra("immersiveQuadScale", "-10")
        c.putExtra("immersiveQuadDistance", "999")
        assertEquals(0.5f, SbsTheaterSettings.scale(c), 0f)
        assertEquals(5f, SbsTheaterSettings.distance(c), 0f)
    }

    @Test fun `configuration survives recreation without enabling Windows VR`() {
        val data = ContainerData(sbsTheaterEnabled = true, windowsVrEnabled = false)
        val saved = with(ContainerData.Saver) { SaverScope { true }.save(data) }!!
        val restored = ContainerData.Saver.restore(saved)!!
        assertTrue(restored.sbsTheaterEnabled)
        assertFalse(restored.windowsVrEnabled)
    }
}
