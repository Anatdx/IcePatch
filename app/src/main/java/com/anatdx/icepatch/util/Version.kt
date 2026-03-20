package com.anatdx.icepatch.util

import android.util.Log
import androidx.core.content.pm.PackageInfoCompat
import com.anatdx.icepatch.APApplication
import com.anatdx.icepatch.BuildConfig
import com.anatdx.icepatch.Natives
import com.anatdx.icepatch.apApp
import java.io.File
import java.io.BufferedReader
import java.io.IOException
import java.io.InputStreamReader


/**
 * version string is like 0.9.0 or 0.9.0-dev
 * version uint is hex number like: 0x000900
 */
object Version {
    private fun parseToolKv(lines: List<String>): Map<String, String> {
        val out = mutableMapOf<String, String>()
        for (line in lines) {
            val trimmed = line.trimEnd()
            if (trimmed.isEmpty()) continue
            val parts = trimmed.split(Regex("\\s+"), limit = 2)
            if (parts.size == 2) {
                out[parts[0]] = parts[1].trim()
            }
        }
        return out
    }

    private fun string2UInt(ver: String): UInt {
        val v = ver.trim().split("-")[0]
        val vn = v.split('.')
        val vi = vn[0].toInt().shl(16) + vn[1].toInt().shl(8) + vn[2].toInt()
        return vi.toUInt()
    }

    fun getKpImg(): String {
        val apd = File(apApp.applicationInfo.nativeLibraryDir, "libapd.so")
        if (!apd.exists()) return "unknown"

        return runCatching {
            val workDir = File(apApp.cacheDir, "kpimg_inspect")
            if (!workDir.exists() && !workDir.mkdirs()) {
                throw IOException("Failed to create temp dir: ${workDir.absolutePath}")
            }

            val kpimg = File(workDir, "kpimg")
            apApp.assets.open("kpimg").use { input ->
                kpimg.outputStream().use { output -> input.copyTo(output) }
            }

            val process = ProcessBuilder(
                apd.path,
                "tool",
                "inspect",
                "--kpimg",
                kpimg.absolutePath,
            ).redirectErrorStream(true).start()

            val lines = mutableListOf<String>()
            BufferedReader(InputStreamReader(process.inputStream)).use { reader ->
                var line: String?
                while (reader.readLine().also { line = it } != null) {
                    line?.let { lines.add(it) }
                }
            }
            val code = process.waitFor()
            if (code != 0) {
                Log.w("APatch", "getKpImg inspect failed, code=$code")
                "unknown"
            } else {
                val kv = parseToolKv(lines)
                kv["kernelpatch_compile"] ?: "unknown"
            }
        }.getOrElse {
            Log.e("APatch", "getKpImg inspect failed", it)
            "unknown"
        }
    }

    fun uInt2String(ver: UInt): String {
        return "%d.%d.%d".format(
            ver.and(0xff0000u).shr(16).toInt(),
            ver.and(0xff00u).shr(8).toInt(),
            ver.and(0xffu).toInt()
        )
    }
    
    fun installedKPTime(): String {
        val time = Natives.kernelPatchBuildTime()
        return if (time.startsWith("ERROR_")) "读取失败" else time
    }

    fun buildKPVUInt(): UInt {
        val buildVS = BuildConfig.buildKPV
        return string2UInt(buildVS)
    }

    fun buildKPVString(): String {
        return BuildConfig.buildKPV
    }

    /**
     * installed KernelPatch version (installed kpimg)
     */
    fun installedKPVUInt(): UInt {
        return Natives.kernelPatchVersion().toUInt()
    }

    fun installedKPVString(): String {
        return uInt2String(installedKPVUInt())
    }


    private fun installedKPatchVString(): String {
        val resultShell = rootShellForResult("${APApplication.APD_PATH} -V")
        val result = resultShell.out.toString()
        return result.trim().ifEmpty { "0" }
    }

    fun installedKPatchVUInt(): UInt {
        return installedKPatchVString().trim().toUInt(0x10)
    }

    private fun installedApdVString(): String {
        fun parseVersion(raw: String): String {
            val matches = Regex("\\d+").findAll(raw).toList()
            return matches.lastOrNull()?.value ?: ""
        }

        val apInfo = getLocalApdInfo()
        val installed = apInfo["ap_installed"] == "true"
        if (!installed) {
            installedApdVString = "0"
            return installedApdVString
        }

        val apdVer = parseVersion(apInfo["ap_version_code"].orEmpty())
        if (apdVer.isNotEmpty()) {
            installedApdVString = apdVer
            Log.i("APatch", "[installedApdVString@Version] apdVer=$installedApdVString")
            return installedApdVString
        }

        installedApdVString = "0"
        Log.i("APatch", "[installedApdVString@Version] status parse fallback=0")
        return installedApdVString
    }

    fun installedApdVUInt(): Int {
        installedApdVInt = installedApdVString().toInt()
        return installedApdVInt
    }


    fun getManagerVersion(): Pair<String, Long> {
        val packageInfo = apApp.packageManager.getPackageInfo(apApp.packageName, 0)!!
        val versionCode = PackageInfoCompat.getLongVersionCode(packageInfo)
        return Pair(packageInfo.versionName!!, versionCode)
    }

    var installedApdVInt: Int = 0
    var installedApdVString: String = "0"
}
