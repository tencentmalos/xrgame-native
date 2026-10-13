package app.gamenative.xrgame

import com.winlator.core.KeyValueSet
import org.junit.Assert.assertEquals
import org.junit.Test

class XrGameMemoryBudgetTest {
    @Test fun unsetOrZeroMeansTheDefault() {
        assertEquals(4096, XrGameMemoryBudget.effectiveMb(null))
        assertEquals(4096, XrGameMemoryBudget.effectiveMb(""))
        assertEquals(4096, XrGameMemoryBudget.effectiveMb("0"))
        assertEquals(4096, XrGameMemoryBudget.effectiveMb("bogus"))
    }

    @Test fun storedValuesStayBetweenTwoAndEightGiB() {
        assertEquals(2048, XrGameMemoryBudget.effectiveMb("512"))
        assertEquals(3072, XrGameMemoryBudget.effectiveMb("3072"))
        assertEquals(8192, XrGameMemoryBudget.effectiveMb("16384"))
    }

    @Test fun withBudgetKeepsTheRestOfTheConfig() {
        val config = "version=2.4,framerate=0,maxDeviceMemory=0,async=1"
        val stored = XrGameMemoryBudget.withBudget(config, 6144)
        val parsed = KeyValueSet(stored)
        assertEquals("6144", parsed.get("maxDeviceMemory"))
        assertEquals("1", parsed.get("async"))
        assertEquals("2.4", parsed.get("version"))
        assertEquals(6144, XrGameMemoryBudget.fromConfig(stored))
        assertEquals(8192, XrGameMemoryBudget.fromConfig(XrGameMemoryBudget.withBudget(config, 99999)))
        assertEquals(4096, XrGameMemoryBudget.fromConfig("version=2.4"))
    }
}
