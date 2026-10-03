package app.gamenative.xrgame

import android.content.Context
import app.gamenative.utils.ManifestInstaller
import com.winlator.container.Container
import com.winlator.contents.ContentProfile.ContentType
import com.winlator.contents.ContentsManager
import com.winlator.core.KeyValueSet
import com.winlator.core.WineUtils
import com.winlator.core.WineRegistryEditor
import com.winlator.core.ProcessHelper
import com.winlator.core.envvars.EnvVars
import com.winlator.xenvironment.ImageFs
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File
import java.util.UUID

/** The v1 runtime selection and installation path, independent of upstream catalogs. */
object XrGameRuntime {
    private val installMutex = Mutex()

    fun configure(container: Container) {
        container.containerVariant = Container.BIONIC
        container.wineVersion = XrGameRuntimeVersions.WINE
        container.setFEXCoreVersion(XrGameRuntimeVersions.FEX)
        container.emulator = "FEXCore"
        // Optional upstream redistributables are not part of our verified catalog.
        // Proton supplies these builtins; DXVK/VKD3D are installed separately.
        container.winComponents = KeyValueSet(container.winComponents).let { components ->
            components.map { it[0] }.forEach { components.put(it, "0") }
            components.toString()
        }
        container.graphicsDriver = "turnip"
        container.displayRenderer = "vulkan"
        container.isUseDRI3 = true
        // The pinned runtime provides both D3D9–11 (DXVK) and D3D12 (VKD3D).
        // A new container's upstream "dxvk" default otherwise leaves Wine's
        // builtin d3d12.dll installed and D3D12 games report no usable adapter.
        container.dxWrapper = "vkd3d"
        val graphics = KeyValueSet(container.graphicsDriverConfig)
        graphics.put("version", XrGameRuntimeVersions.TURNIP)
        graphics.put("adrenotoolsTurnip", "0")
        container.graphicsDriverConfig = graphics.toString()
        val d3d = KeyValueSet(container.dxWrapperConfig)
        d3d.put("version", XrGameRuntimeVersions.DXVK)
        d3d.put("vkd3dVersion", XrGameRuntimeVersions.VKD3D)
        container.dxWrapperConfig = d3d.toString()
    }

    fun configureWindowsComponents(context: Context, container: Container) {
        val systemReg = File(container.rootDir, ".wine/system.reg")
        for (component in KeyValueSet(container.winComponents)) {
            check(component[1] == "0") { "XRGame requires Proton builtins for ${component[0]}" }
            WineUtils.overrideWinComponentDlls(context, container, component[0], false)
            WineUtils.setWinComponentRegistryKeys(systemReg, component[0], false)
        }
    }

    suspend fun prepare(context: Context, container: Container, progress: (Float) -> Unit) =
        withContext(Dispatchers.IO) {
            XrGameProfiler.region("runtime.prepare").use {
            installMutex.withLock {
                configure(container)
                val catalog = XrGameComponents.load(context)
                val base = XrGameComponents.byFileName(context, "imagefs_bionic.txz")
                XrGameProfiler.region("runtime.imagefs.archive").use {
                    XrGameComponents.download(context, base, File(context.filesDir, "imagefs_bionic.txz"), progress)
                }
                XrGameProfiler.region("runtime.imagefs.upgrade").use {
                    XrGameBaseImage.ensure(context, File(context.filesDir, "imagefs_bionic.txz"), requireNotNull(base.sha256))
                }
                val components = listOf(
                    "proton" to (XrGameRuntimeVersions.WINE to ContentType.CONTENT_TYPE_PROTON),
                    "fexcore" to (XrGameRuntimeVersions.FEX to ContentType.CONTENT_TYPE_FEXCORE),
                    "dxvk" to (XrGameRuntimeVersions.DXVK to ContentType.CONTENT_TYPE_DXVK),
                    "vkd3d" to (XrGameRuntimeVersions.VKD3D to ContentType.CONTENT_TYPE_VKD3D),
                )
                for ((kind, selection) in components) {
                    val (version, type) = selection
                    val entry = catalog.items[kind]?.singleOrNull { it.id == version }
                        ?: error("XRGame component is missing from the catalog: $version")
                    val directory = File(ContentsManager.getContentTypeDir(context, type), version)
                    XrGameRuntimeFiles.ensureInstalled(directory,
                        verify = { XrGameProfiler.region("runtime.verify.$kind").use {
                            XrGameInstalledComponents.verify(context, entry, directory)
                        } },
                        beforeRepair = { check(ProcessHelper.listRunningWineProcesses().isEmpty()) {
                            "Close the running game before repairing its runtime"
                        } },
                    ) {
                        val result = ManifestInstaller.downloadAndInstallContent(context, entry, type, progress)
                        check(result.success) { result.message }
                    }
                }
                val driver = catalog.items["driver"]?.singleOrNull { it.id == XrGameRuntimeVersions.TURNIP }
                    ?: error("XRGame Turnip component is missing from the catalog")
                val dir = File(context.filesDir, "contents/adrenotools/${driver.id}")
                XrGameRuntimeFiles.ensureInstalled(dir,
                    verify = { XrGameProfiler.region("runtime.verify.turnip").use {
                        XrGameInstalledComponents.verify(context, driver, dir)
                    } },
                    beforeRepair = { check(ProcessHelper.listRunningWineProcesses().isEmpty()) {
                        "Close the running game before repairing its driver"
                    } },
                ) {
                    val result = ManifestInstaller.downloadAndInstallDriver(context, driver, progress)
                    check(result.success) { result.message }
                }
                check(File(ImageFs.getSharedProtonDir(context), "${XrGameRuntimeVersions.WINE}/bin/wine").isFile) {
                    "XRGame Proton loader is missing"
                }
                // XServerScreen reloads the container from disk after preparation.
                container.saveData()
            }
        }
        }

    /** Install the complete pinned D3D set on every launch, also repairing existing prefixes. */
    fun installGraphics(context: Context, container: Container, manager: ContentsManager) {
        val xrProfile = XrGameProfiler.region("runtime.prefix.graphics")
        try {
        val system32 = File(container.rootDir, ".wine/drive_c/windows/system32")
        val required = mapOf(
            "dxvk-${XrGameRuntimeVersions.DXVK}" to setOf("d3d8.dll", "d3d9.dll", "d3d10core.dll", "d3d11.dll", "dxgi.dll"),
            "vkd3d-${XrGameRuntimeVersions.VKD3D}" to setOf("d3d12.dll", "d3d12core.dll"),
        )
        for ((name, dlls) in required) {
            val profile = manager.getProfileByEntryName(name) ?: error("Bundled graphics component is missing: $name")
            val directory = ContentsManager.getInstallDir(context, profile)
            val files = JSONObject(File(directory, "xrgame-build.json").readText()).getJSONObject("files")
            for (dll in dlls) {
                check(profile.fileList.any { it.source == dll && it.target == "\${system32}/$dll" }) {
                    "Missing graphics installation target: $dll"
                }
                XrGameRuntimeFiles.copyVerified(File(directory, dll), File(system32, dll), files.getJSONObject(dll).getString("sha256"))
            }
        }
        // DXVK supplies d3d10core; the D3D10/10.1 entry points and shader compiler come from Proton.
        val proton = File(ImageFs.getSharedProtonDir(context), XrGameRuntimeVersions.WINE)
        val files = JSONObject(File(proton, "xrgame-build.json").readText()).getJSONObject("files")
        for (dll in listOf("d3d10.dll", "d3d10_1.dll", "d3dcompiler_47.dll")) {
            val source = "lib/wine/aarch64-windows/$dll"
            XrGameRuntimeFiles.copyVerified(File(proton, source), File(system32, dll), files.getJSONObject(source).getString("sha256"))
        }
        WineRegistryEditor(File(container.rootDir, ".wine/user.reg")).use { registry ->
            for (dll in required.values.flatten()) registry.setStringValue("Software\\Wine\\DllOverrides", dll.removeSuffix(".dll"), "native,builtin")
            for (dll in listOf("d3d10", "d3d10_1", "d3dcompiler_47")) registry.setStringValue("Software\\Wine\\DllOverrides", dll, "builtin")
        }
        } finally { xrProfile.close() }
    }

    fun configureGraphics(context: Context, container: Container, env: EnvVars) {
        XrGameProfiler.region("runtime.driver.stage").use {
            val source = File(context.filesDir, "contents/adrenotools/${XrGameRuntimeVersions.TURNIP}")
            val library = File(source, "libvulkan_freedreno.so")
            val record = JSONObject(File(source, "xrgame-build.json").readText())
            val sha = record.getJSONObject("files").getJSONObject(library.name).getString("sha256")
            check(XrGameComponents.verify(library, sha)) { "XRGame Turnip is missing or corrupt" }
            val imageFs = ImageFs.find(context)
            for (name in listOf("libxcb-dri3.so", "libxcb-present.so")) {
                val dependency = File(source, name)
                val dependencySha = record.getJSONObject("files").getJSONObject(name).getString("sha256")
                check(XrGameComponents.verify(dependency, dependencySha)) { "XRGame driver dependency is missing or corrupt: $name" }
                val target = File(imageFs.libDir, name)
                if (!XrGameComponents.verify(target, dependencySha)) dependency.copyTo(target, overwrite = true)
            }
            val dest = File(imageFs.libDir, library.name)
            if (!XrGameComponents.verify(dest, sha)) library.copyTo(dest, overwrite = true)
            val icd = File(imageFs.shareDir, "vulkan/icd.d/freedreno_icd.aarch64.json")
            icd.parentFile?.mkdirs()
            icd.writeText(JSONObject().put("file_format_version", "1.0.0").put("ICD", JSONObject()
                .put("library_path", dest.absolutePath).put("api_version", "1.3.0")).toString())
            env.put("VK_ICD_FILENAMES", icd.absolutePath)
            if (!container.isUseDRI3) env.put("MESA_VK_WSI_DEBUG", "sw")
        }
    }

    /** Apply after user environment merging: this driver has no X11 DRM presentation path. */
    fun configurePresentation(env: EnvVars, turnipVersion: String = XrGameRuntimeVersions.TURNIP) {
        if (turnipVersion == "turnip-d15b7c0-xrg5") {
            env.put("XRGAME_X11_AHB", "1")
            env.remove("MESA_VK_WSI_DEBUG")
            return
        }
        if (turnipVersion != "turnip-d15b7c0-xrg3") return
        fun addFlags(name: String, vararg flags: String) {
            env.put(name, (env.get(name).split(',').map { it.trim() }.filter { it.isNotEmpty() } + flags)
                .distinct().joinToString(","))
        }
        // Keep Turnip GPU rendering; copy completed images through XPutImage until AHB/DRI3 is implemented.
        addFlags("MESA_VK_WSI_DEBUG", "sw", "noshm")
        env.put("MESA_VK_WSI_PRESENT_MODE", "immediate")
        // This XPutImage path emits no Present completion events. VKD3D would wait indefinitely.
        addFlags("VKD3D_DISABLE_EXTENSIONS", "VK_KHR_present_wait", "VK_KHR_present_id")
    }

    /** The caller must finish the stale-Wine process sweep before rotating shared sync state. */
    fun configureSync(directory: File, env: EnvVars, wineRunning: Boolean) {
        check(!wineRunning) { "Cannot rotate XRGame sync state while Wine is running" }
        check(directory.mkdirs() || directory.isDirectory) { "Cannot create XRGame sync directory" }
        directory.listFiles()?.filter { it.isFile && it.name.matches(Regex("session-[a-f0-9-]+(?:\\.v[0-9]+)?\\.shm")) }
            ?.forEach { check(it.delete()) { "Cannot remove inactive XRGame sync state" } }
        env.put("NTSYNC_SHM", File(directory, "session-${UUID.randomUUID()}.shm").absolutePath)
    }
}
