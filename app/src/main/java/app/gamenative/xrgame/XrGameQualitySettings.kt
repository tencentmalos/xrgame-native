package app.gamenative.xrgame

import com.winlator.core.envvars.EnvVars

/**
 * Texture quality, done by the D3D translation layers (vkd3d-proton for D3D12, DXVK for D3D11) and
 * read once when the game starts: sampled textures with mipmaps leave out levels longer than 1024
 * (medium) or 512 (low) texels on their longer side. Block-compressed textures stay in their
 * format. The game keeps seeing its own resource sizes; only the Vulkan images behind them shrink.
 * The default (high) removes the variable, so untouched containers stay as they were.
 *
 * A render scale as in shadPS4 existed until 2026-10-14; it did not suit Steam games and is kept
 * on branch feature/malos/d3d-render-scale.
 */
object XrGameQualitySettings {
    const val TEXTURE_QUALITY_ENV = "XRGAME_TEXTURE_QUALITY"
    val TEXTURE_QUALITIES = listOf("high", "medium", "low")

    fun textureQuality(envVars: String): String =
        EnvVars(envVars).get(TEXTURE_QUALITY_ENV).takeIf { it in TEXTURE_QUALITIES } ?: TEXTURE_QUALITIES[0]

    fun withTextureQuality(envVars: String, quality: String): String = EnvVars(envVars).apply {
        require(quality in TEXTURE_QUALITIES) { "Unknown texture quality: $quality" }
        if (quality == TEXTURE_QUALITIES[0]) remove(TEXTURE_QUALITY_ENV) else put(TEXTURE_QUALITY_ENV, quality)
    }.toString()
}
