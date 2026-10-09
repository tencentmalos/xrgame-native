package app.gamenative.xrgame

import android.content.Context
import androidx.test.core.app.ApplicationProvider
import com.winlator.container.Container
import com.winlator.core.KeyValueSet
import com.winlator.core.WineRegistryEditor
import com.winlator.core.envvars.EnvVars
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import java.io.File

@RunWith(RobolectricTestRunner::class)
class XrGameRuntimeTest {
    @get:Rule val temp = TemporaryFolder()

    @Test fun pinnedX11FallbackOverridesIncompatibleContainerSettingsAndPreservesOtherFlags() {
        val env = EnvVars("MESA_VK_WSI_PRESENT_MODE=mailbox MESA_VK_WSI_DEBUG=linear,sw " +
            "VKD3D_DISABLE_EXTENSIONS=VK_EXT_mesh_shader WINEESYNC=1")
        XrGameRuntime.configurePresentation(env, "turnip-d15b7c0-xrg3")
        val once = env.toString()
        XrGameRuntime.configurePresentation(env, "turnip-d15b7c0-xrg3")
        assertEquals(once, env.toString())
        assertEquals("immediate", env.get("MESA_VK_WSI_PRESENT_MODE"))
        assertEquals("linear,sw,noshm", env.get("MESA_VK_WSI_DEBUG"))
        assertEquals("VK_EXT_mesh_shader,VK_KHR_present_wait,VK_KHR_present_id", env.get("VKD3D_DISABLE_EXTENSIONS"))
        assertEquals("1", env.get("WINEESYNC"))
    }

    @Test fun futureDriverMustSelectItsOwnPresentationPolicy() {
        val env = EnvVars("MESA_VK_WSI_PRESENT_MODE=mailbox")
        XrGameRuntime.configurePresentation(env, "future-turnip")
        assertEquals("MESA_VK_WSI_PRESENT_MODE=mailbox", env.toString())
    }

    @Test fun ahbDriverDoesNotEnableCpuReadbackOrDisablePresentWait() {
        for (driver in listOf("turnip-d15b7c0-xrg5", "turnip-25ef164-xrg6", "turnip-25ef164-xrg10", "turnip-25ef164-xrg11",
                "turnip-04e1d66-xrg12")) {
            val env = EnvVars()
            XrGameRuntime.configurePresentation(env, driver)
            assertEquals("1", env.get("XRGAME_X11_AHB"))
            assertFalse(env.has("MESA_VK_WSI_DEBUG"))
            assertFalse(env.has("VKD3D_DISABLE_EXTENSIONS"))
        }
    }

    @Test fun syncStateRotatesOnlyAfterWineStopsAndPreservesUnrelatedFiles() {
        val dir = temp.newFolder("sync")
        val stale = File(dir, "session-abcd-1234.v9.shm").apply { writeText("stale mutex") }
        val other = File(dir, "unrelated.shm").apply { writeText("keep") }
        val env = EnvVars()
        assertThrows(IllegalStateException::class.java) { XrGameRuntime.configureSync(dir, env, true) }
        assertTrue(stale.exists())
        XrGameRuntime.configureSync(dir, env, false)
        val first = env.get("NTSYNC_SHM")
        assertFalse(stale.exists())
        assertTrue(other.exists())
        assertEquals(dir.canonicalFile, File(first).parentFile.canonicalFile)
        XrGameRuntime.configureSync(dir, env, false)
        assertNotEquals(first, env.get("NTSYNC_SHM"))
    }

    @Test fun upstreamComponentSettingsMigrateWithoutRemovingDxvkOverrides() {
        val context = ApplicationProvider.getApplicationContext<Context>()
        val container = Container("test").apply {
            rootDir = temp.newFolder("prefix")
            winComponents = Container.FALLBACK_WINCOMPONENTS
        }
        val prefix = File(container.rootDir, ".wine").apply { mkdirs() }
        val userReg = File(prefix, "user.reg")
        listOf(userReg, File(prefix, "system.reg")).forEach {
            it.writeText("WINE REGISTRY Version 2\n\n#arch=win64\n")
        }
        val key = "Software\\Wine\\DllOverrides"
        WineRegistryEditor(userReg).use {
            it.setStringValue(key, "d3dcompiler_47", "native,builtin")
            it.setStringValue(key, "msvcr100", "native,builtin")
            it.setStringValue(key, "dxgi", "native,builtin")
        }

        XrGameRuntime.configure(container)
        XrGameRuntime.configureWindowsComponents(context, container)

        assertEquals("vulkan", container.displayRenderer)
        assertEquals("vkd3d", container.dxWrapper)
        assertTrue(container.isUseDRI3)
        assertTrue(KeyValueSet(container.winComponents).all { it[1] == "0" })
        WineRegistryEditor(userReg).use {
            assertNull(it.getStringValue(key, "d3dcompiler_47"))
            assertNull(it.getStringValue(key, "msvcr100"))
            assertEquals("native,builtin", it.getStringValue(key, "dxgi"))
        }
    }
}
