package app.gamenative.powercontrol.drivers

import android.content.Context

/** No proprietary performance SDK is linked into the Pico distribution. */
object OptionalPerformanceDrivers {
    fun create(@Suppress("UNUSED_PARAMETER") context: Context): PerformanceDriver? = null
}
