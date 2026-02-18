package com.anatdx.icepatch.util

import android.content.Context
import android.net.Uri
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Reads KPM (Kernel Patch Module) ELF metadata from the .kpm.info section.
 * Logic mirrors KernelPatch/tools/kpm.c (get_kpm_info / modinfo parsing).
 */
data class KpmInfo(
    val uri: Uri,
    val name: String,
    val version: String,
    val license: String,
    val author: String,
    val description: String
)

object KpmInfoReader {

    private const val ELFMAG = "\u007fELF"
    private const val ET_REL = 1
    private const val EM_AARCH64 = 183
    private const val SIZEOF_ELF64_EHDR = 64
    private const val SIZEOF_ELF64_SHDR = 64
    private const val KPM_INFO_SECTION = ".kpm.info"

    suspend fun readKpmInfo(context: Context, uri: Uri): KpmInfo? = withContext(Dispatchers.IO) {
        runCatching {
            context.contentResolver.openInputStream(uri)?.use { input ->
                val data = input.readBytes()
                parseKpmInfo(uri, data)
            }
        }.getOrNull()
    }

    private fun parseKpmInfo(uri: Uri, data: ByteArray): KpmInfo? {
        if (data.size < SIZEOF_ELF64_EHDR) return null
        val buf = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN)

        // ELF header
        val mag = String(data, 0, 4)
        if (mag != ELFMAG) return null
        val eType = buf.getShort(16)
        val eMachine = buf.getShort(18)
        val eShentsize = buf.getShort(58)
        val eShnum = buf.getShort(60)
        val eShstrndx = buf.getShort(62)
        val eShoff = buf.getLong(40)
        if (eType.toInt() != ET_REL || eMachine.toInt() != EM_AARCH64) return null
        if (eShentsize.toInt() != SIZEOF_ELF64_SHDR) return null
        if (eShoff < 0 || eShnum <= 0 || eShstrndx.toInt() >= eShnum) return null
        if (eShoff + eShnum * SIZEOF_ELF64_SHDR > data.size) return null

        // Section header of string table (section names)
        val strTabIdx = eShstrndx.toInt() and 0xffff
        val strTabOff = eShoff + strTabIdx * SIZEOF_ELF64_SHDR
        val strTabOffset = buf.getLong(strTabOff + 24)
        val strTabSize = buf.getLong(strTabOff + 32)
        if (strTabOffset < 0 || strTabOffset + strTabSize > data.size) return null

        // Find .kpm.info section
        var infoOffset = 0L
        var infoSize = 0L
        for (i in 0 until (eShnum.toInt() and 0xffff)) {
            val shOff = eShoff + i * SIZEOF_ELF64_SHDR
            val shName = buf.getInt(shOff)
            val shOffset = buf.getLong(shOff + 24)
            val shSize = buf.getLong(shOff + 32)
            val name = readCString(data, (strTabOffset + shName).toInt()) ?: continue
            if (name == KPM_INFO_SECTION) {
                infoOffset = shOffset
                infoSize = shSize
                break
            }
        }
        if (infoOffset <= 0 || infoOffset + infoSize > data.size) return null

        // Parse modinfo: NUL-separated "key=value" strings
        val modinfo = data.copyOfRange(infoOffset.toInt(), (infoOffset + infoSize).toInt())
        val fields = parseModinfo(modinfo)
        return KpmInfo(
            uri = uri,
            name = fields["name"] ?: "",
            version = fields["version"] ?: "",
            license = fields["license"] ?: "",
            author = fields["author"] ?: "",
            description = fields["description"] ?: ""
        )
    }

    private fun readCString(data: ByteArray, start: Int): String? {
        if (start < 0 || start >= data.size) return null
        var end = start
        while (end < data.size && data[end].toInt() != 0) end++
        return String(data, start, end - start)
    }

    private fun parseModinfo(data: ByteArray): Map<String, String> {
        val result = mutableMapOf<String, String>()
        var i = 0
        while (i < data.size) {
            val start = i
            while (i < data.size && data[i].toInt() != 0) i++
            if (start < i) {
                val line = String(data, start, i - start).trim()
                if (line.isNotEmpty() && !line.startsWith("#") && '=' in line) {
                    val eq = line.indexOf('=')
                    val key = line.substring(0, eq).trim()
                    val value = line.substring(eq + 1).trim()
                    if (key.isNotEmpty()) result[key] = value
                }
            }
            i++ // skip NUL
        }
        return result
    }
}
