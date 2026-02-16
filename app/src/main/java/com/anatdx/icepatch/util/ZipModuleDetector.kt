package com.anatdx.icepatch.util

import android.content.Context
import android.net.Uri
import com.anatdx.icepatch.R
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.BufferedReader
import java.io.InputStreamReader
import java.util.zip.ZipInputStream

/**
 * Detects APM/Magisk-style module packages inside ZIP files.
 * A valid module contains module.prop at root or in a subdirectory.
 */
data class ModuleZipInfo(
    val uri: Uri,
    val name: String,
    val version: String,
    val description: String,
    val author: String
)

object ZipModuleDetector {

    private const val MODULE_PROP = "module.prop"

    fun isModuleZip(context: Context, uri: Uri): Boolean {
        return try {
            context.contentResolver.openInputStream(uri)?.use { input ->
                ZipInputStream(input).use { zip ->
                    var entry = zip.nextEntry
                    while (entry != null) {
                        val name = entry.name.lowercase()
                        if (name == MODULE_PROP || name.endsWith("/$MODULE_PROP")) {
                            return@use true
                        }
                        zip.closeEntry()
                        entry = zip.nextEntry
                    }
                    false
                }
            } ?: false
        } catch (e: Exception) {
            e.printStackTrace()
            false
        }
    }

    fun parseModuleInfo(context: Context, uri: Uri): ModuleZipInfo {
        var name = context.getString(R.string.zip_module_unknown)
        var version = ""
        var description = ""
        var author = ""

        try {
            context.contentResolver.openInputStream(uri)?.use { input ->
                ZipInputStream(input).use { zip ->
                    var entry = zip.nextEntry
                    while (entry != null) {
                        val entryName = entry.name.lowercase()
                        if (entryName == MODULE_PROP || entryName.endsWith("/$MODULE_PROP")) {
                            BufferedReader(InputStreamReader(zip)).use { reader ->
                                reader.forEachLine { line ->
                                    if (line.contains("=") && !line.startsWith("#")) {
                                        val (key, value) = line.split("=", limit = 2)
                                            .map { it.trim() }
                                        when (key) {
                                            "name" -> name = value
                                            "version" -> version = value
                                            "description" -> description = value
                                            "author" -> author = value
                                        }
                                    }
                                }
                            }
                            break
                        }
                        zip.closeEntry()
                        entry = zip.nextEntry
                    }
                }
            }
        } catch (e: Exception) {
            e.printStackTrace()
        }

        return ModuleZipInfo(uri, name, version, description, author)
    }

    suspend fun detectModuleZips(context: Context, uris: List<Uri>): List<ModuleZipInfo> =
        withContext(Dispatchers.IO) {
            uris.mapNotNull { uri ->
                if (isModuleZip(context, uri)) parseModuleInfo(context, uri) else null
            }
        }
}
