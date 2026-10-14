package app.gamenative.xrgame

import com.winlator.core.envvars.EnvVars

/**
 * Memory reductions done by the D3D translation layers (vkd3d-proton for D3D12, DXVK for D3D9–11),
 * read once when the game starts. The game keeps seeing its own resource sizes; only the Vulkan
 * images behind them shrink.
 * - Texture quality: sampled textures with mipmaps leave out levels longer than 1024 (medium) or
 *   512 (low) texels on their longer side. Block-compressed textures stay in their format.
 * - Render scale, as shadPS4's: render and depth targets, back buffers included, are rendered at
 *   75%, 50%, 37.5% or 25% of their size; shaders get their coordinates mapped and presentation
 *   stretches the frame back.
 * The defaults (high, 100%) remove the variables, so untouched containers stay as they were.
 */
object XrGameQualitySettings {
    const val TEXTURE_QUALITY_ENV = "XRGAME_TEXTURE_QUALITY"
    const val RENDER_SCALE_ENV = "XRGAME_RENDER_SCALE"
    val TEXTURE_QUALITIES = listOf("high", "medium", "low")
    /** Percentages as the translation layers read them; shadPS4's eighths 8, 6, 4, 3 and 2. */
    val RENDER_SCALES = listOf("100", "75", "50", "37.5", "25")

    fun textureQuality(envVars: String): String =
        EnvVars(envVars).get(TEXTURE_QUALITY_ENV).takeIf { it in TEXTURE_QUALITIES } ?: TEXTURE_QUALITIES[0]

    fun withTextureQuality(envVars: String, quality: String): String = EnvVars(envVars).apply {
        require(quality in TEXTURE_QUALITIES) { "Unknown texture quality: $quality" }
        if (quality == TEXTURE_QUALITIES[0]) remove(TEXTURE_QUALITY_ENV) else put(TEXTURE_QUALITY_ENV, quality)
    }.toString()

    fun renderScale(envVars: String): String =
        EnvVars(envVars).get(RENDER_SCALE_ENV).takeIf { it in RENDER_SCALES } ?: RENDER_SCALES[0]

    fun withRenderScale(envVars: String, percent: String): String = EnvVars(envVars).apply {
        require(percent in RENDER_SCALES) { "Unknown render scale: $percent" }
        if (percent == RENDER_SCALES[0]) remove(RENDER_SCALE_ENV) else put(RENDER_SCALE_ENV, percent)
    }.toString()
}
