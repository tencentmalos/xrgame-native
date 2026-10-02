package app.gamenative.ui.screen.xr.sbs

object SbsNative {
    init { System.loadLibrary("xrimmersive") }
    external fun create(endpoint: String): Long
    external fun draw(handle: Long, width: Int, height: Int): Long
    external fun destroy(handle: Long)
}
