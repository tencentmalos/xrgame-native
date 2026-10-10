package app.gamenative.powercontrol.metrics

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class SystemMetricsSourcesTest {
    @get:Rule val folder = TemporaryFolder()

    @Test fun readsMillidegreesFromTheFirstUsableNode() {
        val garbage = folder.newFile("garbage").apply { writeText("n/a\n") }
        val zone = folder.newFile("zone_temp").apply { writeText("45678\n") }
        assertEquals(46, SystemMetricsSources.readTemperatureC(listOf(garbage.path, zone.path)))
    }

    @Test fun aNodeThatFailedToOpenIsNotRetried() {
        val refused = File(folder.root, "kgsl_temp")
        val zone = folder.newFile("thermal_zone_temp").apply { writeText("52000") }
        assertEquals(52, SystemMetricsSources.readTemperatureC(listOf(refused.path, zone.path)))

        refused.writeText("80000")
        assertEquals(52, SystemMetricsSources.readTemperatureC(listOf(refused.path, zone.path)))
        assertNull(SystemMetricsSources.readTemperatureC(listOf(refused.path)))
    }
}
