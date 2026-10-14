package app.gamenative.data

data class UserFilesUploadResult(
    val uploadBatchSuccess: Boolean,
    val appChangeNumber: Long,
    val filesUploaded: Int,
    val bytesUploaded: Long,
    // XRGame: local deletes the cloud confirmed (the batch only declares them otherwise).
    val filesDeleted: List<UserFileInfo> = emptyList(),
)
