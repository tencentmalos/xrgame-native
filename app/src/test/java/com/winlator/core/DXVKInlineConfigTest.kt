package com.winlator.core

import com.winlator.core.envvars.EnvVars
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.RuntimeEnvironment
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [33], manifest = Config.NONE)
class DXVKInlineConfigTest {
    @Test
    fun `raw exec environment preserves separate DXVK options and quoted descriptions`() {
        org.junit.Assume.assumeTrue(app.gamenative.BuildConfig.XRGAME)
        val env = EnvVars()
        DXVKHelper.setEnvVars(RuntimeEnvironment.getApplication(), KeyValueSet(
            "maxDeviceMemory=4096,maxFeatureLevel=11_1,customDevice=1234:5678:Test GPU"
        ), env)
        val raw = env.get("DXVK_CONFIG")
        assertFalse(raw.startsWith('"'))
        assertFalse(raw.contains('\n'))
        assertTrue(raw.contains("dxgi.maxDeviceMemory = 4096;dxgi.maxSharedMemory = 4096;"))
        assertTrue(raw.contains("d3d11.maxFeatureLevel = 11_1;"))
        assertTrue(raw.contains("dxgi.customDeviceDesc = \"Test GPU\";"))
        assertEquals(raw, EnvVars(env.toString()).get("DXVK_CONFIG"))
    }
}
