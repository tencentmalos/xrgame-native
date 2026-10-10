package app.gamenative.xrgame

import app.gamenative.data.UserFileInfo
import com.google.protobuf.ByteString
import `in`.dragonbra.javasteam.enums.EResult
import `in`.dragonbra.javasteam.protobufs.steamclient.SteammessagesCloudSteamclient.CCloud_ClientBeginFileUpload_Request
import `in`.dragonbra.javasteam.protobufs.steamclient.SteammessagesCloudSteamclient.CCloud_ClientCommitFileUpload_Request
import `in`.dragonbra.javasteam.protobufs.steamclient.SteammessagesCloudSteamclient.CCloud_ClientDeleteFile_Request
import `in`.dragonbra.javasteam.rpc.service.Cloud
import `in`.dragonbra.javasteam.steam.handlers.steamcloud.FileUploadBlockDetails
import `in`.dragonbra.javasteam.steam.handlers.steamunifiedmessages.SteamUnifiedMessages
import `in`.dragonbra.javasteam.steam.steamclient.SteamClient
import java.util.Date
import okhttp3.RequestBody
import okhttp3.RequestBody.Companion.toRequestBody
import timber.log.Timber

/**
 * Steam Cloud upload calls that keep the service EResult. JavaSteam's SteamCloud wrappers drop it,
 * so a refused ClientBeginFileUpload looks like a file that needs no blocks and its commit then
 * fails without a reason. Also carries out the per-file deletes that an upload batch declares;
 * declaring them in BeginAppUploadBatch alone leaves the files in the cloud. Signed storage URLs
 * and request headers stay out of the log.
 */
object XrGameCloudRpc {
    class BeginResult(val result: EResult, val encryptFile: Boolean, val blocks: List<FileUploadBlockDetails>)

    /** Cloud deletes are skipped above this many files, or above half of the files synced before. */
    const val MAX_DELETES = 8

    /** The Cloud service, or null where the client has no unified messages (unit tests mock SteamCloud). */
    fun service(client: SteamClient?): Cloud? =
        client?.getHandler(SteamUnifiedMessages::class.java)?.createService(Cloud::class.java)

    suspend fun beginFileUpload(
        cloud: Cloud,
        cellId: Int,
        appId: Int,
        filename: String,
        fileSize: Int,
        fileSha: ByteArray,
        timestamp: Date,
        uploadBatchId: Long,
    ): BeginResult {
        val request = CCloud_ClientBeginFileUpload_Request.newBuilder().apply {
            this.appid = appId
            this.fileSize = fileSize
            this.rawFileSize = fileSize
            this.fileSha = ByteString.copyFrom(fileSha)
            this.timeStamp = timestamp.time / 1000L
            this.filename = filename
            this.platformsToSync = -1
            this.cellId = cellId
            // The upload path sends the plain file; never let Steam ask for an encrypted one.
            this.canEncrypt = false
            this.isSharedFile = false
            this.uploadBatchId = uploadBatchId
        }.build()
        val response = cloud.clientBeginFileUpload(request).await()
        val body = response.body
        val blocks = body.blockRequestsList.map { FileUploadBlockDetails(it) }
        Timber.i(
            "Steam Cloud begin upload %s: %s encrypt=%b blocks=[%s]",
            filename,
            response.result,
            body.encryptFile,
            blocks.joinToString { "${methodName(it.httpMethod)} ${it.blockLength}B@${it.blockOffset} body=${it.explicitBodyData.size}B" },
        )
        return BeginResult(response.result, body.encryptFile, blocks)
    }

    suspend fun commitFileUpload(cloud: Cloud, appId: Int, filename: String, fileSha: ByteArray, transferSucceeded: Boolean): Boolean {
        val request = CCloud_ClientCommitFileUpload_Request.newBuilder().apply {
            this.transferSucceeded = transferSucceeded
            this.appid = appId
            this.fileSha = ByteString.copyFrom(fileSha)
            this.filename = filename
        }.build()
        val response = cloud.clientCommitFileUpload(request).await()
        Timber.i(
            "Steam Cloud commit %s: %s committed=%b transferSucceeded=%b",
            filename,
            response.result,
            response.body.fileCommitted,
            transferSucceeded,
        )
        return response.result == EResult.OK && response.body.fileCommitted
    }

    suspend fun deleteFile(cloud: Cloud, appId: Int, filename: String, uploadBatchId: Long): Boolean {
        val request = CCloud_ClientDeleteFile_Request.newBuilder().apply {
            this.appid = appId
            this.filename = filename
            this.isExplicitDelete = false
            this.uploadBatchId = uploadBatchId
        }.build()
        val response = cloud.clientDeleteFile(request).await()
        Timber.i("Steam Cloud delete %s: %s", filename, response.result)
        return response.result == EResult.OK
    }

    /**
     * Whether files that disappeared locally may be deleted from the cloud. A scan that found
     * nothing, or one that lost most of what was synced before, points at a path problem rather
     * than the game removing its files, so the cloud copies stay.
     */
    fun allowDeletes(deleted: List<UserFileInfo>, local: List<UserFileInfo>, synced: List<UserFileInfo>): Boolean {
        if (deleted.isEmpty()) return true
        val allowed = local.isNotEmpty() && deleted.size <= MAX_DELETES && deleted.size * 2 <= synced.size
        if (!allowed) {
            Timber.w(
                "Steam Cloud: keeping %d cloud file(s) that are missing locally (local=%d, synced before=%d)",
                deleted.size,
                local.size,
                synced.size,
            )
        }
        return allowed
    }

    /** EHTTPMethod of a block request; unknown values keep the historical PUT. */
    fun methodName(method: Int): String = when (method) {
        1 -> "GET"
        2 -> "HEAD"
        3 -> "POST"
        4 -> "PUT"
        5 -> "DELETE"
        6 -> "OPTIONS"
        7 -> "PATCH"
        else -> "PUT"
    }

    /** The block's explicit body when Steam sends one (e.g. a multipart completion), else the file bytes. */
    fun blockBody(block: FileUploadBlockDetails, fileBytes: ByteArray, contentType: okhttp3.MediaType?): RequestBody? =
        when (methodName(block.httpMethod)) {
            "GET", "HEAD" -> null
            else -> (if (block.explicitBodyData.isNotEmpty()) block.explicitBodyData else fileBytes).toRequestBody(contentType)
        }
}
