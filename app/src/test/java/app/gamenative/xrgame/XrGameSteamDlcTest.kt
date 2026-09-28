package app.gamenative.xrgame

import app.gamenative.data.*
import app.gamenative.enums.OS
import app.gamenative.enums.OSArch
import `in`.dragonbra.javasteam.enums.*
import java.util.Date
import java.util.EnumSet
import org.junit.Assert.*
import org.junit.Test

class XrGameSteamDlcTest {
    private val base = SteamApp(id = 582010)
    private fun dlc(id: Int) = SteamApp(id = id, name = "DLC $id", dlcForAppId = base.id)
    private fun license(ids: List<Int>, vararg flags: ELicenseFlags) = SteamLicense(
        packageId = 1, lastChangeNumber = 0, timeCreated = Date(0), timeNextProcess = Date(0),
        minuteLimit = 0, minutesUsed = 0, paymentMethod = EPaymentMethod.None,
        licenseFlags = EnumSet.noneOf(ELicenseFlags::class.java).apply { addAll(flags) },
        purchaseCode = "", licenseType = ELicenseType.SinglePurchase, territoryCode = 0,
        accessToken = 0, ownerAccountId = emptyList(), masterPackageID = 0, appIds = ids)

    @Test fun iceborneUsesTheLicenseEvenWhenMissingFromParentsOldDlcList() {
        val iceborne = dlc(1118010)
        val unowned = dlc(1118011)
        val otherGame = dlc(2).copy(dlcForAppId = 123)
        assertEquals(mapOf(iceborne.id to iceborne.name), XrGameSteamDlc.available(base,
            listOf(iceborne, unowned, otherGame), listOf(license(listOf(iceborne.id, otherGame.id),
                ELicenseFlags.RegionRestrictionExpired)), emptySet(), "public"))
    }

    @Test fun catalogAndInvalidGrantsCannotUnlockDlcButAnIndependentValidGrantCan() {
        val dlc = dlc(1118010)
        for (flag in listOf(ELicenseFlags.Pending, ELicenseFlags.Expired, ELicenseFlags.CancelledByUser,
            ELicenseFlags.CancelledByAdmin, ELicenseFlags.CancelledByFriendlyFraudLock,
            ELicenseFlags.NotActivated, ELicenseFlags.CancelledByPartner)) {
            val invalid = license(listOf(dlc.id), flag)
            assertTrue(XrGameSteamDlc.available(base, listOf(dlc), listOf(invalid), emptySet(), "public").isEmpty())
            assertEquals(setOf(dlc.id), XrGameSteamDlc.available(base, listOf(dlc),
                listOf(invalid, license(listOf(dlc.id))), emptySet(), "public").keys)
        }
        assertTrue(XrGameSteamDlc.available(base, listOf(dlc), emptyList(), emptySet(), "public").isEmpty())
        assertTrue(XrGameSteamDlc.available(base, listOf(dlc),
            listOf(license(listOf(dlc.id)).copy(licenseType = ELicenseType.NoLicense)), emptySet(), "public").isEmpty())
    }

    @Test fun ownedTexturePackNeedsItsDepotEvenWhenDlcMetadataHasNoDepots() {
        val texture = dlc(960781)
        val depot = DepotInfo(depotId = 582012, dlcAppId = texture.id, depotFromApp = Int.MAX_VALUE,
            sharedInstall = false, osList = EnumSet.of(OS.windows), osArch = OSArch.Unknown,
            manifests = mapOf("public" to ManifestInfo(name = "public", gid = 1, size = 1, download = 1)),
            encryptedManifests = emptyMap())
        val game = base.copy(depots = mapOf(depot.depotId to depot))
        val grants = listOf(license(listOf(texture.id)))
        assertTrue(XrGameSteamDlc.available(game, listOf(texture), grants, setOf(582011), "public").isEmpty())
        assertEquals(setOf(texture.id), XrGameSteamDlc.available(game, listOf(texture), grants,
            setOf(582011, depot.depotId), "public").keys)
    }
}
