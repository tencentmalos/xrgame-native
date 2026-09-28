package app.gamenative.powercontrol.drivers

import android.content.Context

object OptionalPerformanceDrivers {
    fun create(context: Context): PerformanceDriver? {
        if (!SamsungPerformanceDriver.isSamsungDevice()) return null
        return SamsungPerformanceDriver(context).takeIf { it.isDriverSupported() } ?: NoOpPerformanceDriver()
    }
}
