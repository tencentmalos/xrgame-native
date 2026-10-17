// SPDX-License-Identifier: GPL-3.0-or-later
package app.gamenative.xrgame.steam

import `in`.dragonbra.javasteam.types.KeyValue
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import java.io.File
import java.nio.file.Files
import java.nio.file.StandardCopyOption

/** Steam's on-disk installation record. Never infer installed versions from current appinfo. */
object SteamLibraryManifest {
    data class Install(val appId: Int, val directory: File, val complete: Boolean,
                       val depots: Map<Int, String>, val branch: String, val size: Long)

    fun read(file: File): Install? = runCatching {
        if (!file.isFile || file.length() > 2 * 1024 * 1024) return null
        val id = Regex("appmanifest_([1-9][0-9]*)\\.acf").matchEntire(file.name)?.groupValues?.get(1)?.toIntOrNull() ?: return null
        val kv = KeyValue.loadFromString(file.readText()) ?: return null
        if (kv.name != "AppState" || kv["appid"].value?.toIntOrNull() != id) return null
        val name = kv["installdir"].value ?: return null
        if (!safeName(name)) return null
        val common = File(file.parentFile, "common").canonicalFile
        val directory = File(common, name).canonicalFile
        // Library imports are real directories. Links outside the library need an explicit route.
        if (!directory.toPath().startsWith(common.toPath()) || directory == common) return null
        val depots = kv["InstalledDepots"].children.mapNotNull { depot ->
            val depotId = depot.name?.toIntOrNull()?.takeIf { it > 0 } ?: return@mapNotNull null
            val manifest = depot["manifest"].value?.takeIf(::validManifest) ?: return@mapNotNull null
            depotId to manifest
        }.toMap()
        val complete = kv["StateFlags"].value == "4" && directory.isDirectory && depots.isNotEmpty() &&
            !File(directory, ".download_in_progress").exists() &&
            !File(file.parentFile, "downloading/$id").exists()
        Install(id, directory, complete, depots, kv["UserConfig"]["BetaKey"].value.orEmpty().ifBlank { "public" },
            kv["SizeOnDisk"].value?.toLongOrNull()?.coerceAtLeast(0) ?: 0)
    }.getOrNull()

    fun safeName(name: String) = name.isNotBlank() && name !in setOf(".", "..") &&
        name.none { it == '/' || it == '\\' || it == ':' || it.isISOControl() }

    private fun validManifest(value: String) = value.toULongOrNull()?.let {
        it != 0uL && it != 0x7fffffffffffffffuL
    } == true

    fun installedDepots(directory: File): Map<Int, String>? = runCatching {
        if (!File(directory, ".download_complete").isFile || File(directory, ".download_in_progress").exists()) return null
        val config = File(directory, ".DepotDownloader/depot.config")
        if (!config.isFile || config.length() > 1024 * 1024) return null
        // Android JSONObject converts unsigned 64-bit IDs to Double and loses bits.
        val ids = Json.parseToJsonElement(config.readText()).jsonObject.getValue("installedManifestIDs").jsonObject
        val result = linkedMapOf<Int, String>()
        for ((key, value) in ids) {
            val id = key.toIntOrNull()?.takeIf { it > 0 } ?: return null
            val manifest = value.jsonPrimitive.content
            if (!validManifest(manifest)) return null
            result[id] = manifest
        }
        result.takeIf { it.isNotEmpty() }
    }.getOrNull()

    fun export(appId: Int, name: String, directory: File, branch: String, language: String): KeyValue? {
        require(appId > 0 && safeName(directory.name))
        val depots = installedDepots(directory) ?: return null
        return KeyValue("AppState").apply {
            put("appid", appId.toString()); put("Universe", "1"); put("name", name)
            put("StateFlags", "4"); put("installdir", directory.name)
            // The downloader did not persist a build ID. Zero lets Steam verify/update honestly.
            put("buildid", "0"); put("AutoUpdateBehavior", "1")
            child("InstalledDepots").apply { depots.toSortedMap().forEach { (id, manifest) -> child(id.toString()).put("manifest", manifest) } }
            child("UserConfig").apply {
                put("language", language); put("platform_override_source", "linux"); put("platform_override_dest", "windows")
                if (branch != "public") put("BetaKey", branch)
            }
        }
    }

    internal fun KeyValue.child(key: String): KeyValue = children.firstOrNull { it.name.equals(key, true) }
        ?: KeyValue(key).also { children.add(it) }
    internal fun KeyValue.put(key: String, text: String) { child(key).value = text }

    internal fun write(file: File, kv: KeyValue) {
        check(file.parentFile!!.isDirectory || file.parentFile!!.mkdirs())
        val temp = File(file.parentFile, ".${file.name}.${java.util.UUID.randomUUID()}.tmp")
        try {
            kv.saveToFile(temp, false)
            try { Files.move(temp.toPath(), file.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE) }
            catch (_: java.nio.file.AtomicMoveNotSupportedException) { Files.move(temp.toPath(), file.toPath(), StandardCopyOption.REPLACE_EXISTING) }
        } finally { temp.delete() }
    }
}
