package com.anatdx.icepatch.util

import android.util.Log
import androidx.core.content.pm.PackageInfoCompat
import com.anatdx.icepatch.APApplication
import com.anatdx.icepatch.BuildConfig
import com.anatdx.icepatch.Natives
import com.anatdx.icepatch.apApp
import com.anatdx.icepatch.util.shellForResult
import com.anatdx.icepatch.util.withNewRootShell
import com.topjohnwu.superuser.ShellUtils
import org.ini4j.Ini
import java.io.StringReader
import com.anatdx.icepatch.ui.viewmodel.KPModel
import com.topjohnwu.superuser.nio.ExtendedFile
import com.topjohnwu.superuser.nio.FileSystemManager
import com.topjohnwu.superuser.Shell
import androidx.compose.runtime.mutableStateOf
import java.io.File
import android.system.Os


/**
 * version string is like 0.9.0 or 0.9.0-dev
 * version uint is hex number like: 0x000900
 */
object Version {

    private fun string2UInt(ver: String): UInt {
        val v = ver.trim().split("-")[0]
        val vn = v.split('.')
        val vi = vn[0].toInt().shl(16) + vn[1].toInt().shl(8) + vn[2].toInt()
        return vi.toUInt()
    }

    fun getKpImg(): String {
        var shell: Shell = createRootShell()
        var kimgInfo = mutableStateOf(KPModel.KImgInfo("", false))
        var kpimgInfo = mutableStateOf(KPModel.KPImgInfo("", "", "", "", ""))
        val patchDir: ExtendedFile = FileSystemManager.getLocal().getFile(apApp.filesDir.parent, "check")
        patchDir.deleteRecursively()
        patchDir.mkdirs()
        val execs = listOf(
            "libkptools.so", "libmagiskboot.so", "libbusybox.so"
        )

        val info = apApp.applicationInfo
        val libs = File(info.nativeLibraryDir).listFiles { _, name ->
            execs.contains(name)
        } ?: emptyArray()

        for (lib in libs) {
            val name = lib.name.substring(3, lib.name.length - 3)
            Os.symlink(lib.path, "$patchDir/$name")
        }

        for (script in listOf(
            "boot_patch.sh", "boot_unpatch.sh", "boot_extract.sh", "util_functions.sh", "kpimg"
        )) {
            val dest = File(patchDir, script)
            apApp.assets.open(script).writeTo(dest)
        }
        val result = shellForResult(
            shell, "cd $patchDir", "./kptools -l -k kpimg"
        )

        if (result.isSuccess) {
            val ini = Ini(StringReader(result.out.joinToString("\n")))
            val kpimg = ini["kpimg"]
            if (kpimg != null) {
                kpimgInfo.value = KPModel.KPImgInfo(
                    kpimg["version"].toString(),
                    kpimg["compile_time"].toString(),
                    kpimg["config"].toString(),
                    APApplication.superKey,     // current key
                    kpimg["root_superkey"].toString()      // possibly empty
                )
                return kpimg["compile_time"].toString()
            } 
        } 

        return "unknown"
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

        fun rootCmd(cmd: String): String {
            return withNewRootShell { ShellUtils.fastCmd(this, cmd) }.trim()
        }

        fun rootOk(cmd: String): Boolean {
            return withNewRootShell { ShellUtils.fastCmdResult(this, cmd) }
        }

        val hasApd = rootOk("[ -x ${APApplication.APD_PATH} ]")
        if (!hasApd) {
            installedApdVString = "0"
            return installedApdVString
        }

        val apdVer = parseVersion(rootCmd("${APApplication.APD_PATH} -V"))
        if (apdVer.isNotEmpty()) {
            installedApdVString = apdVer
            Log.i("APatch", "[installedApdVString@Version] apdVer=$installedApdVString")
            return installedApdVString
        }

        val fileVer = parseVersion(rootCmd("cat ${APApplication.APATCH_VERSION_PATH}"))
        installedApdVString = if (fileVer.isNotEmpty()) fileVer else "0"
        Log.i("APatch", "[installedApdVString@Version] fileVer=$installedApdVString")
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
