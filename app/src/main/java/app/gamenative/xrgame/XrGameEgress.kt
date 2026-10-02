package app.gamenative.xrgame

import java.io.IOException
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.Proxy
import java.net.ProxySelector
import java.net.SocketAddress
import java.net.URI
import java.util.concurrent.ConcurrentHashMap
import okhttp3.OkHttpClient
import timber.log.Timber

/**
 * Process-wide egress allowlist for XRGame Native (spec C3, WP1-6).
 *
 * OkHttp and HttpURLConnection ask the default [ProxySelector] for every connection. For hosts
 * outside the allowlist this selector returns an HTTP proxy on a closed loopback port, so the
 * request fails at once, as if offline, and the target host is never resolved or contacted.
 * Callers see an ordinary IOException and fall back to their offline behavior.
 *
 * Each blocked host is logged once with the tag "XrGameEgress". Those log lines are the
 * inventory of upstream-service calls that WP1/WP2 still have to switch off at the source.
 *
 * Not covered, by design: raw sockets (the ProxySelector only applies SOCKS proxies to them),
 * JavaSteam's CM connections, and the Rust download engine. All of those talk only to Valve.
 * Verify it with a per-UID connection capture on the device, not with this log.
 */
object XrGameEgress {
    private var inheritedSelector: ProxySelector? = null
    private var installedSelector: ProxySelector? = null

    // Inherited route for catalog-validated components and the CM-scoped Cloud client below.
    fun componentProxySelector(): ProxySelector = inheritedSelector ?: object : ProxySelector() {
        override fun select(uri: URI?) = listOf(Proxy.NO_PROXY)
        override fun connectFailed(uri: URI?, sa: SocketAddress?, ioe: IOException?) = Unit
    }
    /** Valve-operated domains: Steam CM, Web API, store/community, content CDN, cloud, images. */
    private val allowedSuffixes = listOf(
        "steampowered.com",
        "steamcommunity.com",
        "steamstatic.com",
        "steamcontent.com",
        "steamserver.net",
        "steamusercontent.com",
        "steamgames.com",
        "steamchina.com",
        "valvesoftware.com",
        "steamcdn-a.akamaihd.net",
        "steamcommunity-a.akamaihd.net",
        "steamuserimages-a.akamaihd.net",
        "steampipe.akamaized.net",
    )

    private val blackhole = Proxy(Proxy.Type.HTTP, InetSocketAddress(InetAddress.getLoopbackAddress(), 9))
    private val loggedHosts = ConcurrentHashMap.newKeySet<String>()

    // These exact storage endpoints were returned by authenticated SteamCloud CM replies.
    // They are not generally allowed: only the request built from that reply gets this route.
    private val steamCloudStorageHosts = setOf(
        "steamcloud-hkg.oss-accelerate.aliyuncs.com",
        "steamcloud-sgp.oss-accelerate.aliyuncs.com",
        "steamcloudhk2.blob.core.windows.net",
        "steamcloud-tyo.s3.dualstack.ap-northeast-1.amazonaws.com",
    )

    fun steamCloudClient(base: OkHttpClient, authorizedUrl: String): OkHttpClient {
        val uri = URI(authorizedUrl)
        if (uri.scheme != "https" || uri.userInfo != null || uri.port !in listOf(-1, 443) ||
            uri.host?.lowercase() !in steamCloudStorageHosts) return base
        val route = componentProxySelector()
        val selector = object : ProxySelector() {
            override fun select(target: URI?): List<Proxy> =
                if (target?.scheme == "https" && target.host.equals(uri.host, ignoreCase = true) &&
                    target.userInfo == null && target.port in listOf(-1, 443)) route.select(target)
                else listOf(blackhole)

            override fun connectFailed(target: URI?, sa: SocketAddress?, ioe: IOException?) {
                route.connectFailed(target, sa, ioe)
            }
        }
        // A signed Cloud URL cannot grant a redirect access to another origin.
        return base.newBuilder().proxy(null).proxySelector(selector)
            .followRedirects(false).followSslRedirects(false).build()
    }

    fun isAllowed(host: String?): Boolean {
        if (host.isNullOrEmpty()) return true
        val h = host.lowercase().trimEnd('.').removePrefix("[").removeSuffix("]")
        if (h == "localhost" || isIpLiteral(h)) return true
        return allowedSuffixes.any { h == it || h.endsWith(".$it") }
    }

    // Loopback and LAN addresses are local IPC or user-chosen; public IP literals are rare
    // (CM servers are reached over raw sockets), so they are allowed but still logged.
    private fun isIpLiteral(h: String): Boolean {
        val looksLikeIp = h.all { it.isDigit() || it == '.' } || h.contains(':')
        if (!looksLikeIp) return false
        val addr = runCatching { InetAddress.getByName(h) }.getOrNull() ?: return false
        if (!(addr.isLoopbackAddress || addr.isSiteLocalAddress || addr.isLinkLocalAddress) && loggedHosts.add(h)) {
            Timber.tag("XrGameEgress").i("allowed public IP literal %s", h)
        }
        return true
    }

    @Synchronized
    fun install() {
        val previous = ProxySelector.getDefault()
        if (previous != null && previous === installedSelector) return
        inheritedSelector = previous
        ProxySelector.setDefault(
            object : ProxySelector() {
                override fun select(uri: URI?): List<Proxy> {
                    val host = uri?.host
                    if (isAllowed(host)) return previous?.select(uri) ?: listOf(Proxy.NO_PROXY)
                    if (loggedHosts.add(host!!)) {
                        Timber.tag("XrGameEgress").w("blocked %s (%s)", host, uri.scheme)
                    }
                    return listOf(blackhole)
                }

                override fun connectFailed(uri: URI?, sa: SocketAddress?, ioe: IOException?) {
                    previous?.connectFailed(uri, sa, ioe)
                }
            },
        )
        installedSelector = ProxySelector.getDefault()
        Timber.tag("XrGameEgress").i("installed; allowed suffixes: %s", allowedSuffixes)
    }
}
