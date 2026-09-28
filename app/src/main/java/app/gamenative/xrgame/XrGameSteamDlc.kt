package app.gamenative.xrgame

import app.gamenative.data.SteamApp
import app.gamenative.data.SteamLicense
import `in`.dragonbra.javasteam.enums.ELicenseFlags
import `in`.dragonbra.javasteam.enums.ELicenseType

/** Catalog metadata alone is not ownership. Never synthesize grants from PICS access tokens. */
object XrGameSteamDlc {
    private val inactive = setOf(ELicenseFlags.Pending, ELicenseFlags.Expired,
        ELicenseFlags.CancelledByUser, ELicenseFlags.CancelledByAdmin,
        ELicenseFlags.CancelledByFriendlyFraudLock, ELicenseFlags.NotActivated,
        ELicenseFlags.CancelledByPartner)

    fun available(base: SteamApp, dlcs: List<SteamApp>, licenses: List<SteamLicense>,
                  installedDepots: Set<Int>, branch: String): Map<Int, String> {
        val owned = licenses.filter { it.licenseType != ELicenseType.NoLicense &&
            it.licenseFlags.none(inactive::contains) }.flatMap { it.appIds }.toSet()
        return dlcs.filter { dlc ->
            dlc.id > 0 && dlc.id != base.id && dlc.dlcForAppId == base.id && dlc.id in owned &&
                // Content-only entitlements (including Iceborne) need no separate download.
                // Depot-bearing DLC is exposed only after its content was installed.
                (base.depots.values.filter { it.dlcAppId == dlc.id } + dlc.depots.values)
                    .filter { it.isWindowsCompatible && branch in it.manifests }
                    .let { depots -> depots.isEmpty() || depots.any { it.depotId in installedDepots } }
        }.sortedBy { it.id }.associate { it.id to it.name }
    }
}
