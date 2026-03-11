package com.anatdx.icepatch.ui.viewmodel

import android.content.ContentValues
import android.content.Context
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import android.system.Os
import android.util.Log
import androidx.annotation.RequiresApi
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.core.content.FileProvider
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.topjohnwu.superuser.CallbackList
import com.topjohnwu.superuser.Shell
import com.topjohnwu.superuser.nio.ExtendedFile
import com.topjohnwu.superuser.nio.FileSystemManager
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import com.anatdx.icepatch.APApplication
import com.anatdx.icepatch.BuildConfig
import com.anatdx.icepatch.R
import com.anatdx.icepatch.apApp
import com.anatdx.icepatch.util.Version
import com.anatdx.icepatch.util.copyAndClose
import com.anatdx.icepatch.util.copyAndCloseOut
import com.anatdx.icepatch.util.createRootShell
import com.anatdx.icepatch.util.inputStream
import com.anatdx.icepatch.util.KpmInfoReader
import com.anatdx.icepatch.util.shellForResult
import com.anatdx.icepatch.util.writeTo
import java.io.BufferedReader
import java.io.File
import java.io.FileNotFoundException
import java.io.IOException
import java.io.InputStreamReader

private const val TAG = "PatchViewModel"
private const val PATCH_WORKSPACE_STAMP_FILE = ".patch_workspace_rev"
private const val PATCH_WORKSPACE_REV = "2026-03-11-apd-native-bootflow-v1"

class PatchesViewModel : ViewModel() {
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

    private fun parseExtraType(type: String): KPModel.ExtraType {
        return KPModel.ExtraType.entries.firstOrNull { it.desc.equals(type, ignoreCase = true) }
            ?: KPModel.ExtraType.NONE
    }

    private fun parseEmbeddedExtras(lines: List<String>): List<KPModel.IExtraInfo> {
        val kv = parseToolKv(lines)
        val indexRegex = Regex("""extra\[(\d+)]\.""")
        val indexes = mutableSetOf<Int>()
        for (key in kv.keys) {
            val m = indexRegex.find(key) ?: continue
            indexes.add(m.groupValues[1].toIntOrNull() ?: continue)
        }
        if (indexes.isEmpty()) return emptyList()

        val extras = mutableListOf<KPModel.IExtraInfo>()
        for (idx in indexes.sorted()) {
            val prefix = "extra[$idx]."
            val type = parseExtraType(kv[prefix + "type"] ?: "none")
            if (type == KPModel.ExtraType.NONE) continue
            val name = kv[prefix + "name"] ?: continue
            val event = kv[prefix + "event"] ?: ""
            val args = kv[prefix + "args"] ?: ""
            if (type == KPModel.ExtraType.KPM) {
                extras.add(
                    KPModel.KPMInfo(
                        type = type,
                        name = name,
                        event = event,
                        args = args,
                        version = kv[prefix + "kpm.version"] ?: "",
                        license = kv[prefix + "kpm.license"] ?: "",
                        author = kv[prefix + "kpm.author"] ?: "",
                        description = kv[prefix + "kpm.description"] ?: "",
                    )
                )
            } else {
                extras.add(
                    KPModel.GenericExtraInfo(
                        type = type,
                        name = name,
                        event = event,
                        args = args,
                    )
                )
            }
        }
        return extras
    }

    private fun buildExtraPatchArgs(): List<String> {
        val args = mutableListOf<String>()

        for (extra in existedExtras) {
            if (extra.name.isBlank() || extra.type == KPModel.ExtraType.NONE) continue
            args.add("--embeded-extra-name")
            args.add(extra.name)
            args.add("--extra-type")
            args.add(extra.type.desc)
            if (extra.event.isNotBlank()) {
                args.add("--extra-event")
                args.add(extra.event)
            }
            if (extra.args.isNotBlank()) {
                args.add("--extra-args")
                args.add(extra.args)
            }
        }

        val count = minOf(newExtras.size, newExtrasFileName.size)
        for (i in 0 until count) {
            val extra = newExtras[i]
            if (extra.type == KPModel.ExtraType.NONE) continue
            args.add("--embed-extra-path")
            args.add(newExtrasFileName[i])
            args.add("--extra-type")
            args.add(extra.type.desc)
            if (extra.name.isNotBlank()) {
                args.add("--extra-name")
                args.add(extra.name)
            }
            if (extra.event.isNotBlank()) {
                args.add("--extra-event")
                args.add(extra.event)
            }
            if (extra.args.isNotBlank()) {
                args.add("--extra-args")
                args.add(extra.args)
            }
        }
        return args
    }

    enum class PatchMode(val sId: Int) {
        PATCH_ONLY(R.string.patch_mode_bootimg_patch),
        PATCH_AND_INSTALL(R.string.patch_mode_patch_and_install),
        INSTALL_TO_NEXT_SLOT(R.string.patch_mode_install_to_next_slot),
        UNPATCH(R.string.patch_mode_uninstall_patch)
    }

    var bootSlot by mutableStateOf("")
    var bootDev by mutableStateOf("")
    var kimgInfo by mutableStateOf(KPModel.KImgInfo("", false))
    var kpimgInfo by mutableStateOf(KPModel.KPImgInfo("", "", "", "", ""))
    var superkey by mutableStateOf(APApplication.superKey)
    var existedExtras = mutableStateListOf<KPModel.IExtraInfo>()
    var newExtras = mutableStateListOf<KPModel.IExtraInfo>()
    var newExtrasFileName = mutableListOf<String>()

    var running by mutableStateOf(false)
    var patching by mutableStateOf(false)
    var patchdone by mutableStateOf(false)
    var needReboot by mutableStateOf(false)

    var error by mutableStateOf("")
    var patchLog by mutableStateOf("")

    private val patchDir: ExtendedFile = FileSystemManager.getLocal().getFile(apApp.filesDir.parent, "patch")
    private var srcBoot: ExtendedFile = patchDir.getChildFile("boot.img")
    private var shell: Shell = createRootShell()
    private var prepared: Boolean = false
    private var lastPreparedMode: PatchMode? = null

    private fun shellQuote(value: String): String {
        return "'" + value.replace("'", "'\"'\"'") + "'"
    }

    private fun resolveApdCommandInPatchDir(): String {
        val nativeApd = File(apApp.applicationInfo.nativeLibraryDir, "libapd.so")
        if (nativeApd.exists()) return nativeApd.path
        val localApd = File(patchDir.path, "apd")
        if (localApd.exists()) return "./apd"
        val localLibApd = File(patchDir.path, "libapd.so")
        if (localLibApd.exists()) return "./libapd.so"
        return APApplication.APD_PATH
    }

    private fun resolveApdOverridePath(): String {
        val nativeApd = File(apApp.applicationInfo.nativeLibraryDir, "libapd.so")
        if (nativeApd.exists()) return nativeApd.path
        val localApd = File(patchDir.path, "apd")
        if (localApd.exists()) return localApd.path
        val localLibApd = File(patchDir.path, "libapd.so")
        if (localLibApd.exists()) return localLibApd.path
        return APApplication.APD_PATH
    }

    private data class ProcessRunResult(
        val code: Int,
        val lines: List<String>,
    ) {
        val isSuccess: Boolean get() = code == 0
    }

    private fun buildApdShellCommand(apdPath: String, args: List<String>): String {
        val full = mutableListOf(apdPath)
        full.addAll(args)
        return full.joinToString(" ") { shellQuote(it) }
    }

    private fun runApdRoot(apdPath: String, args: List<String>, logs: CallbackList<String>): Shell.Result {
        val cmd = buildApdShellCommand(apdPath, args)
        return shell.newJob().add(
            "export ASH_STANDALONE=1",
            "cd ${shellQuote(patchDir.path)}",
            cmd,
        ).to(logs, logs).exec()
    }

    private fun runApdLocal(apdPath: String, args: List<String>, logs: CallbackList<String>): ProcessRunResult {
        val command = mutableListOf(apdPath)
        command.addAll(args)
        val builder = ProcessBuilder(command)
        builder.environment()["ASH_STANDALONE"] = "1"
        builder.directory(File(patchDir.path))
        builder.redirectErrorStream(true)
        val process = builder.start()
        val lines = mutableListOf<String>()
        BufferedReader(InputStreamReader(process.inputStream)).use { reader ->
            var line: String?
            while (reader.readLine().also { line = it } != null) {
                val s = line ?: ""
                lines.add(s)
                logs.add(s)
            }
        }
        val code = process.waitFor()
        return ProcessRunResult(code, lines)
    }

    private fun runShell(cmd: String): Shell.Result = shellForResult(shell, cmd)

    private fun detectCurrentSlotSuffix(): String? {
        val cmdline = shellForResult(shell, "cat /proc/cmdline 2>/dev/null").out.joinToString(" ")
        val bootconfig = shellForResult(shell, "cat /proc/bootconfig 2>/dev/null").out.joinToString(" ")
        val merged = "$cmdline $bootconfig"
        var suffix = Regex("""(?:^|\s)androidboot\.slot_suffix=([^\s]+)""")
            .find(merged)?.groupValues?.getOrNull(1).orEmpty()

        if (suffix.isBlank()) {
            val slot = Regex("""(?:^|\s)androidboot\.slot=([^\s]+)""")
                .find(merged)?.groupValues?.getOrNull(1).orEmpty()
            if (slot.isNotBlank() && slot != "normal") {
                suffix = if (slot.startsWith("_")) slot else "_$slot"
            }
        }

        if (suffix.isBlank()) {
            val propSlot = shellForResult(shell, "getprop ro.boot.slot_suffix").out
                .joinToString("")
                .trim()
            if (propSlot.isNotBlank() && propSlot != "normal") {
                suffix = if (propSlot.startsWith("_")) propSlot else "_$propSlot"
            }
        }

        return suffix.ifBlank { null }
    }

    private fun findBlockByName(name: String): String? {
        val findResult = runShell("find /dev/block \\( -type b -o -type c -o -type l \\) -iname ${shellQuote(name)} 2>/dev/null | head -n 1")
        if (!findResult.isSuccess) return null
        val raw = findResult.out.firstOrNull()?.trim().orEmpty()
        if (raw.isBlank()) return null
        val resolved = runShell("readlink -f ${shellQuote(raw)}").out.firstOrNull()?.trim().orEmpty()
        return if (resolved.isNotBlank()) resolved else raw
    }

    private fun findBootImageDevice(slotSuffix: String?): String? {
        val candidates = mutableListOf<String>()
        if (!slotSuffix.isNullOrBlank()) {
            candidates += "boot$slotSuffix"
        }
        candidates += listOf("kern-a", "android_boot", "kernel", "bootimg", "boot", "lnx", "boot_a")
        for (name in candidates.distinct()) {
            val path = findBlockByName(name)
            if (!path.isNullOrBlank()) return path
        }
        val fromFstab = runShell(
            "grep -v '#' /etc/*fstab* 2>/dev/null | grep -E '/boot(img)?[^a-zA-Z]' | grep -oE '/dev/[a-zA-Z0-9_./-]*' | head -n 1",
        ).out.firstOrNull()?.trim().orEmpty()
        return fromFstab.ifBlank { null }
    }

    private fun resolveTargetBoot(nextSlot: Boolean): Pair<String, String>? {
        val current = detectCurrentSlotSuffix()
        val target = if (!nextSlot) {
            current
        } else {
            when (current) {
                "_a" -> "_b"
                "_b" -> "_a"
                else -> null
            }
        }
        if (nextSlot && target == null) return null
        val bootDevice = findBootImageDevice(target)
        if (bootDevice.isNullOrBlank()) return null
        return Pair(target.orEmpty(), bootDevice)
    }

    private fun flashBootImage(newBootPath: String, bootDevice: String, logs: CallbackList<String>): Shell.Result {
        val cmd = buildString {
            append("if [ -b ")
            append(shellQuote(bootDevice))
            append(" ] || [ -c ")
            append(shellQuote(bootDevice))
            append(" ]; then ")
            append("blockdev --setrw ")
            append(shellQuote(bootDevice))
            append(" 2>/dev/null || true; ")
            append("dd if=")
            append(shellQuote(newBootPath))
            append(" of=")
            append(shellQuote(bootDevice))
            append(" bs=4096 iflag=fullblock conv=notrunc,fsync; ")
            append("sync; ")
            append("else cp ")
            append(shellQuote(newBootPath))
            append(" ")
            append(shellQuote(bootDevice))
            append("; fi")
        }
        return shell.newJob().add(cmd).to(logs, logs).exec()
    }

    private fun ensurePatchOnlyWorkspacePrepared() {
        val dir = File(patchDir.path)
        val apdExists = File(dir, "apd").exists() || File(dir, "libapd.so").exists()
        val assetsReady = File(dir, "kpimg").exists()
        val workspaceRev = runCatching { File(dir, PATCH_WORKSPACE_STAMP_FILE).readText().trim() }
            .getOrDefault("")
        val workspaceCurrent = workspaceRev == PATCH_WORKSPACE_REV
        if (!dir.exists() || !apdExists || !assetsReady || !workspaceCurrent) {
            prepare()
        } else {
            dir.mkdirs()
        }
        if (kpimgInfo.version.isEmpty()) {
            parseKpimg()
        }
        prepared = true
        lastPreparedMode = PatchMode.PATCH_ONLY
    }

    private fun prepare() {
        patchDir.deleteRecursively()
        patchDir.mkdirs()
        val execs = listOf(
            "libapd.so", "libbusybox.so", "libkpatch.so", "libbootctl.so"
        )
        error = ""

        val info = apApp.applicationInfo
        val libs = File(info.nativeLibraryDir).listFiles { _, name ->
            execs.contains(name)
        } ?: emptyArray()

        for (lib in libs) {
            val name = lib.name.substring(3, lib.name.length - 3)
            val out = File(patchDir.path, name)
            runCatching {
                if (out.exists()) out.delete()
                lib.copyTo(out, overwrite = true)
                out.setExecutable(true, false)
            }.onFailure {
                runCatching {
                    if (out.exists()) out.delete()
                    Os.symlink(lib.path, out.path)
                }
            }
        }

        // Extract required assets
        for (asset in listOf("kpimg")) {
            val dest = File(patchDir, asset)
            apApp.assets.open(asset).writeTo(dest)
        }
        runCatching {
            File(patchDir.path, PATCH_WORKSPACE_STAMP_FILE).writeText(PATCH_WORKSPACE_REV)
        }

    }

    private fun parseKpimg() {
        val apdCmd = resolveApdCommandInPatchDir()
        val cdCmd = "cd ${shellQuote(patchDir.path)}"
        val result = shellForResult(
            shell, cdCmd, "${shellQuote(apdCmd)} tool inspect --kpimg kpimg"
        )

        if (result.isSuccess) {
            val kv = parseToolKv(result.out)
            kpimgInfo = KPModel.KPImgInfo(
                kv["kernelpatch_version"] ?: "",
                kv["kernelpatch_compile"] ?: "",
                kv["kernelpatch_config"] ?: "",
                APApplication.superKey,
                ""
            )
        } else {
            error = result.err.joinToString("\n")
        }
    }

    private fun parseBootimg(bootimg: String) {
        val apdCmd = resolveApdCommandInPatchDir()
        val cdCmd = "cd ${shellQuote(patchDir.path)}"
        val bootArg = shellQuote(bootimg)
        val unpackResult = shellForResult(
            shell,
            cdCmd,
            "${shellQuote(apdCmd)} tool boot unpack --boot $bootArg --out-kernel kernel",
        )
        if (!unpackResult.isSuccess) {
            error += unpackResult.err.joinToString("\n")
            return
        }

        val inspectResult = shellForResult(
            shell,
            cdCmd,
            "${shellQuote(apdCmd)} tool inspect --image kernel",
        )
        if (!inspectResult.isSuccess) {
            error += inspectResult.err.joinToString("\n")
            return
        }

        val kv = parseToolKv(inspectResult.out)
        Log.d(TAG, "kernel image info: $kv")
        kimgInfo = KPModel.KImgInfo(
            kv["kernel_version"] ?: (kv["file"] ?: ""),
            kv["kernelpatch_patched"].toBoolean()
        )
        existedExtras.clear()

        val listResult = shellForResult(
            shell,
            cdCmd,
            "${shellQuote(apdCmd)} tool list --image kernel",
        )
        if (listResult.isSuccess) {
            existedExtras.addAll(parseEmbeddedExtras(listResult.out))
        } else {
            Log.w(TAG, "parse embedded extras failed: ${listResult.err.joinToString("\\n")}")
        }
    }

    val checkSuperKeyValidation: (superKey: String) -> Boolean = { superKey ->
        superKey.length in 8..63 && superKey.any { it.isDigit() } && superKey.any { it.isLetter() }
    }

    fun copyAndParseBootimg(uri: Uri) {
        viewModelScope.launch(Dispatchers.IO) {
            if (running) return@launch
            running = true
            ensurePatchOnlyWorkspacePrepared()
            try {
                uri.inputStream().buffered().use { src ->
                    srcBoot.also {
                        src.copyAndCloseOut(it.newOutputStream())
                    }
                }
            } catch (e: IOException) {
                Log.e(TAG, "copy boot image error: $e")
                error = "Copy boot image failed: ${e.message ?: e.javaClass.simpleName}"
                running = false
                return@launch
            }
            parseBootimg(srcBoot.path)
            running = false
        }
    }

    private fun extractAndParseBootimg(mode: PatchMode) {
        if (!shell.isRoot) {
            error = "Direct install requires root. Please use Select File mode."
            running = false
            return
        }
        val target = resolveTargetBoot(mode == PatchMode.INSTALL_TO_NEXT_SLOT)
        if (target == null) {
            error = if (mode == PatchMode.INSTALL_TO_NEXT_SLOT) {
                "Failed to locate next-slot boot partition."
            } else {
                "Failed to locate boot partition."
            }
            running = false
            return
        }
        bootSlot = target.first
        bootDev = target.second
        Log.i(TAG, "current slot: $bootSlot")
        Log.i(TAG, "current bootimg: $bootDev")
        srcBoot = FileSystemManager.getLocal().getFile(bootDev)
        parseBootimg(bootDev)
        running = false
    }

    fun prepare(mode: PatchMode) {
        viewModelScope.launch(Dispatchers.IO) {
            val needExtract = mode == PatchMode.PATCH_AND_INSTALL || mode == PatchMode.UNPATCH || mode == PatchMode.INSTALL_TO_NEXT_SLOT
            val modeChanged = lastPreparedMode != mode
            val hasPatchOnlyBoot = mode == PatchMode.PATCH_ONLY && File(srcBoot.path).exists()

            if (!prepared) {
                prepared = true
                running = true
                if (mode == PatchMode.PATCH_ONLY) {
                    ensurePatchOnlyWorkspacePrepared()
                } else if (!hasPatchOnlyBoot) {
                    prepare()
                } else {
                    patchDir.mkdirs()
                }
                if (mode != PatchMode.UNPATCH) {
                    parseKpimg()
                }
                if (needExtract) {
                    extractAndParseBootimg(mode)
                } else if (mode == PatchMode.PATCH_ONLY && hasPatchOnlyBoot && kimgInfo.banner.isEmpty()) {
                    parseBootimg(srcBoot.path)
                }
                lastPreparedMode = mode
                running = false
            } else if (modeChanged) {
                running = true
                error = ""
                when (mode) {
                    PatchMode.UNPATCH -> {
                        kpimgInfo = KPModel.KPImgInfo("", "", "", "", "")
                        existedExtras.clear()
                        extractAndParseBootimg(mode)
                    }
                    PatchMode.PATCH_AND_INSTALL, PatchMode.INSTALL_TO_NEXT_SLOT -> {
                        existedExtras.clear()
                        parseKpimg()
                        extractAndParseBootimg(mode)
                    }
                    PatchMode.PATCH_ONLY -> {
                        bootSlot = ""
                        bootDev = ""
                        existedExtras.clear()
                        parseKpimg()
                        if (File(srcBoot.path).exists()) {
                            parseBootimg(srcBoot.path)
                        } else {
                            kimgInfo = KPModel.KImgInfo("", false)
                        }
                    }
                }
                lastPreparedMode = mode
                running = false
            }
            if (mode == PatchMode.PATCH_AND_INSTALL) {
                pendingEmbedKpmUri?.let { uri ->
                    pendingEmbedKpmUri = null
                    embedKPM(uri)
                }
            }
        }
    }

    var pendingEmbedKpmUri: Uri? = null

    fun clearCache() {
        prepared = false
        lastPreparedMode = null
        bootSlot = ""
        bootDev = ""
        kimgInfo = KPModel.KImgInfo("", false)
        kpimgInfo = KPModel.KPImgInfo("", "", "", "", "")
        error = ""
        existedExtras.clear()
        newExtras.clear()
        newExtrasFileName.clear()
        pendingEmbedKpmUri = null
        running = false
        patching = false
        patchdone = false
        needReboot = false
        patchLog = ""
    }

    fun embedKPM(uri: Uri) {
        viewModelScope.launch(Dispatchers.IO) {
            if (running) return@launch
            running = true
            error = ""

            val rand = (1..4).map { ('a'..'z').random() }.joinToString("")
            val kpmFileName = "${rand}.kpm"
            val kpmFile: ExtendedFile = patchDir.getChildFile(kpmFileName)

            Log.i(TAG, "copy kpm to: " + kpmFile.path)
            try {
                uri.inputStream().buffered().use { src ->
                    kpmFile.also {
                        src.copyAndCloseOut(it.newOutputStream())
                    }
                }
            } catch (e: IOException) {
                Log.e(TAG, "Copy kpm error: $e")
            }

            val info = KpmInfoReader.readKpmInfo(apApp, uri)
            if (info != null) {
                val kpmInfo = KPModel.KPMInfo(
                    KPModel.ExtraType.KPM,
                    info.name,
                    KPModel.TriggerEvent.PRE_KERNEL_INIT.event,
                    "",
                    info.version,
                    info.license,
                    info.author,
                    info.description,
                )
                newExtras.add(kpmInfo)
                newExtrasFileName.add(kpmFileName)
            } else {
                error = "Invalid KPM\n"
            }
            running = false
        }
    }

    fun doUnpatch() {
        viewModelScope.launch(Dispatchers.IO) {
            patching = true
            patchLog = ""
            Log.i(TAG, "starting unpatching...")

            val logs = object : CallbackList<String>() {
                override fun onAddElement(e: String?) {
                    patchLog += e
                    Log.i(TAG, "" + e)
                    patchLog += "\n"
                }
            }

            val apdPath = resolveApdOverridePath()
            logs.add("- Using apd binary: $apdPath")

            var success = false
            val hasBackup = shellForResult(
                shell,
                "[ -f /data/adb/ap/ori.img ] && echo true || echo false",
            ).out.firstOrNull()?.trim() == "true"

            if (hasBackup) {
                logs.add("- Found backup ori.img, using it directly")
                val copyBackup = shell.newJob().add(
                    "cd ${shellQuote(patchDir.path)}",
                    "cp /data/adb/ap/ori.img new-boot.img",
                ).to(logs, logs).exec()
                success = copyBackup.isSuccess
            } else {
                logs.add("- Unpacking boot image")
                val unpack = runApdRoot(
                    apdPath,
                    listOf("tool", "boot", "unpack", "--boot", bootDev, "--out-kernel", "kernel"),
                    logs,
                )
                if (unpack.isSuccess) {
                    val inspect = runApdRoot(
                        apdPath,
                        listOf("tool", "inspect", "--image", "kernel"),
                        logs,
                    )
                    if (!inspect.isSuccess) {
                        error = inspect.err.joinToString("\n")
                    } else {
                        val kv = parseToolKv(inspect.out)
                        val alreadyUnpatched = kv["kernelpatch_patched"] == "false"
                        if (alreadyUnpatched) {
                            logs.add("- No need to unpatch: kernel already clean")
                            success = true
                        } else {
                            logs.add("- Unpatching kernel")
                            val unpatch = runApdRoot(
                                apdPath,
                                listOf("tool", "unpatch", "--image", "kernel", "--out", "kernel.clean"),
                                logs,
                            )
                            if (unpatch.isSuccess) {
                                logs.add("- Repacking boot image")
                                val repack = runApdRoot(
                                    apdPath,
                                    listOf(
                                        "tool",
                                        "boot",
                                        "repack",
                                        "--boot",
                                        bootDev,
                                        "--kernel",
                                        "kernel.clean",
                                        "--out",
                                        "new-boot.img",
                                    ),
                                    logs,
                                )
                                success = repack.isSuccess
                                if (!repack.isSuccess) {
                                    error = repack.err.joinToString("\n")
                                }
                            } else {
                                error = unpatch.err.joinToString("\n")
                            }
                        }
                    }
                } else {
                    error = unpack.err.joinToString("\n")
                }
            }

            if (success) {
                val hasNewBoot = shellForResult(
                    shell,
                    "cd ${shellQuote(patchDir.path)} && [ -f new-boot.img ] && echo true || echo false",
                ).out.firstOrNull()?.trim() == "true"
                if (hasNewBoot) {
                    logs.add("- Flashing boot image")
                    val flash = flashBootImage("${patchDir.path}/new-boot.img", bootDev, logs)
                    success = flash.isSuccess
                    if (!flash.isSuccess) {
                        error = flash.err.joinToString("\n")
                    }
                }
            }

            val cleanup = shell.newJob().add(
                "rm -f ${shellQuote(APApplication.APD_PATH)}",
                "rm -rf ${shellQuote(APApplication.APATCH_FOLDER)}",
            ).to(logs, logs).exec()
            if (!cleanup.isSuccess && error.isBlank()) {
                error = cleanup.err.joinToString("\n")
            }

            if (success) {
                logs.add(" Unpatch successful")
                needReboot = true
                APApplication.markNeedReboot()
            } else {
                logs.add(" Unpatched failed")
                if (error.isBlank()) error = "Unpatch failed."
            }
            logs.add("****************************")

            patchdone = true
            patching = false
        }
    }
    fun isSuExecutable(): Boolean {
        val suFile = File("/system/bin/su")
        return suFile.exists() && suFile.canExecute()
    }
    fun doPatch(mode: PatchMode) {
        viewModelScope.launch(Dispatchers.IO) {
            patching = true
            Log.d(TAG, "starting patching...")

            val apVer = Version.getManagerVersion().second
            val rand = (1..4).map { ('a'..'z').random() }.joinToString("")
            val outFilename = "apatch_patched_${apVer}_${BuildConfig.buildKPV}_${rand}.img"

            val logs = object : CallbackList<String>() {
                override fun onAddElement(e: String?) {
                    patchLog += e
                    Log.d(TAG, "" + e)
                    patchLog += "\n"
                }
            }
            logs.add("****************************")

            val requiresRootInstallMode =
                mode == PatchMode.PATCH_AND_INSTALL || mode == PatchMode.INSTALL_TO_NEXT_SLOT
            if (requiresRootInstallMode && !shell.isRoot) {
                error = "Direct install requires root. Please use Select File mode."
                logs.add(error)
                logs.add("****************************")
                patchdone = true
                patching = false
                return@launch
            }
            val bootImageArg = srcBoot.path
            val apdPath = resolveApdOverridePath()
            val extraArgs = buildExtraPatchArgs()

            if (mode == PatchMode.PATCH_ONLY && !File(bootImageArg).exists()) {
                error = "Selected boot image does not exist: $bootImageArg"
                logs.add(error)
                logs.add("****************************")
                patchdone = true
                patching = false
                return@launch
            }

            logs.add("- Using boot image: $bootImageArg")
            logs.add("- Using apd binary: $apdPath")

            fun runApd(args: List<String>, failPrefix: String): Boolean {
                return if (requiresRootInstallMode) {
                    val result = runApdRoot(apdPath, args, logs)
                    if (!result.isSuccess) {
                        val errText = result.err.joinToString("\n")
                        error = if (errText.isNotBlank()) "$failPrefix: $errText" else "$failPrefix (code ${result.code})"
                    }
                    result.isSuccess
                } else {
                    val result = runApdLocal(apdPath, args, logs)
                    if (!result.isSuccess) {
                        error = buildString {
                            append("$failPrefix (exit code ${result.code})")
                            if (result.lines.isNotEmpty()) {
                                append('\n')
                                append(result.lines.takeLast(30).joinToString("\n"))
                            }
                        }
                    }
                    result.isSuccess
                }
            }

            var succ = runApd(
                listOf("tool", "boot", "unpack", "--boot", bootImageArg, "--out-kernel", "kernel.ori"),
                "Unpack boot image failed",
            )

            if (succ) {
                val inspectOut: List<String>? = if (requiresRootInstallMode) {
                    val inspect = runApdRoot(apdPath, listOf("tool", "inspect", "--image", "kernel.ori"), logs)
                    if (!inspect.isSuccess) {
                        succ = false
                        val errText = inspect.err.joinToString("\n")
                        error = if (errText.isNotBlank()) {
                            "Inspect kernel failed: $errText"
                        } else {
                            "Inspect kernel failed (code ${inspect.code})"
                        }
                        null
                    } else {
                        inspect.out
                    }
                } else {
                    val inspect = runApdLocal(apdPath, listOf("tool", "inspect", "--image", "kernel.ori"), logs)
                    if (!inspect.isSuccess) {
                        succ = false
                        error = buildString {
                            append("Inspect kernel failed (exit code ${inspect.code})")
                            if (inspect.lines.isNotEmpty()) {
                                append('\n')
                                append(inspect.lines.takeLast(30).joinToString("\n"))
                            }
                        }
                        null
                    } else {
                        inspect.lines
                    }
                }
                if (succ && inspectOut != null) {
                    val kv = parseToolKv(inspectOut)
                    if (kv["kernelpatch_patched"] == "false") {
                        logs.add("- Backing boot image")
                        if (requiresRootInstallMode) {
                            val backup = shell.newJob().add(
                                "cd ${shellQuote(patchDir.path)}",
                                "cp ${shellQuote(bootImageArg)} ori.img",
                            ).to(logs, logs).exec()
                            succ = backup.isSuccess
                            if (!succ) {
                                error = "Backup boot image failed: ${backup.err.joinToString("\n")}"
                            }
                        } else {
                            runCatching {
                                File(bootImageArg).copyTo(File(patchDir.path, "ori.img"), overwrite = true)
                            }.onFailure {
                                succ = false
                                error = "Backup boot image failed: ${it.message ?: it.javaClass.simpleName}"
                            }
                        }
                    }
                }
            }

            if (succ) {
                val patchArgs = mutableListOf(
                    "tool",
                    "patch",
                    "--image",
                    "kernel.ori",
                    "--kpimg",
                    "kpimg",
                    "--out",
                    "kernel",
                    "--skey",
                    superkey,
                )
                patchArgs.addAll(extraArgs)
                succ = runApd(patchArgs, "Patch kernel failed")
            }

            if (succ) {
                succ = runApd(
                    listOf("tool", "boot", "repack", "--boot", bootImageArg, "--kernel", "kernel", "--out", "new-boot.img"),
                    "Repack boot image failed",
                )
            }

            if (succ && requiresRootInstallMode) {
                logs.add("- Flashing new boot image")
                val flash = flashBootImage("${patchDir.path}/new-boot.img", bootImageArg, logs)
                succ = flash.isSuccess
                if (!succ) {
                    error = "Flash boot image failed: ${flash.err.joinToString("\n")}"
                }
            }

            if (!succ) {
                logs.add(error)
                logs.add("****************************")
                patchdone = true
                patching = false
                return@launch
            }

            if (mode == PatchMode.PATCH_AND_INSTALL) {
                logs.add("- Reboot to finish the installation...")
                needReboot = true
                APApplication.markNeedReboot()
            } else if (mode == PatchMode.INSTALL_TO_NEXT_SLOT) {
                logs.add("- Connecting boot hal...")
                val bootctlStatus = shell.newJob().add(
                    "cd $patchDir", "chmod 0777 $patchDir/bootctl", "./bootctl hal-info"
                ).to(logs, logs).exec()
                if (!bootctlStatus.isSuccess) {
                    logs.add("[X] Failed to connect to boot hal, you may need switch slot manually")
                } else {
                    val currSlot = shellForResult(
                        shell, "cd $patchDir", "./bootctl get-current-slot"
                    ).out.toString()
                    val targetSlot = if (currSlot.contains("0")) {
                        1
                    } else {
                        0
                    }
                    logs.add("- Switching to next slot: $targetSlot...")
                    val setNextActiveSlot = shell.newJob().add(
                        "cd $patchDir", "./bootctl set-active-boot-slot $targetSlot"
                    ).exec()
                    if (setNextActiveSlot.isSuccess) {
                        logs.add("- Switch done")
                        logs.add("- Writing boot marker script...")
                        val markBootableScript = shell.newJob().add(
                            "mkdir -p /data/adb/post-fs-data.d && rm -rf /data/adb/post-fs-data.d/post_ota.sh && touch /data/adb/post-fs-data.d/post_ota.sh",
                            "echo \"chmod 0777 $patchDir/bootctl\" > /data/adb/post-fs-data.d/post_ota.sh",
                            "echo \"chown root:root 0777 $patchDir/bootctl\" > /data/adb/post-fs-data.d/post_ota.sh",
                            "echo \"$patchDir/bootctl mark-boot-successful\" > /data/adb/post-fs-data.d/post_ota.sh",
                            "echo >> /data/adb/post-fs-data.d/post_ota.sh",
                            "echo \"rm -rf $patchDir\" >> /data/adb/post-fs-data.d/post_ota.sh",
                            "echo >> /data/adb/post-fs-data.d/post_ota.sh",
                            "echo \"rm -f /data/adb/post-fs-data.d/post_ota.sh\" >> /data/adb/post-fs-data.d/post_ota.sh",
                            "chmod 0777 /data/adb/post-fs-data.d/post_ota.sh",
                            "chown root:root /data/adb/post-fs-data.d/post_ota.sh",
                        ).to(logs, logs).exec()
                        if (markBootableScript.isSuccess) {
                            logs.add("- Boot marker script write done")
                        } else {
                            logs.add("[X] Boot marker scripts write failed")
                        }
                    }
                }
                logs.add("- Reboot to finish the installation...")
                needReboot = true
                APApplication.markNeedReboot()
            } else if (mode == PatchMode.PATCH_ONLY) {
                val newBootFile = patchDir.getChildFile("new-boot.img")
                val outDir = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS)
                if (!outDir.exists()) outDir.mkdirs()
                val outPath = File(outDir, outFilename)
                val inputUri = newBootFile.getUri(apApp)

                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    val outUri = createDownloadUri(apApp, outFilename)
                    succ = insertDownload(apApp, outUri, inputUri)
                } else {
                    newBootFile.inputStream().copyAndClose(outPath.outputStream())
                }
                if (succ) {
                    logs.add(" Output file is written to ")
                    logs.add(" ${outPath.path}")
                } else {
                    logs.add(" Write patched boot.img failed")
                }
            }
            logs.add("****************************")
            patchdone = true
            patching = false
        }
    }

    @RequiresApi(Build.VERSION_CODES.Q)
    fun createDownloadUri(context: Context, outFilename: String): Uri? {
        val contentValues = ContentValues().apply {
            put(MediaStore.Downloads.DISPLAY_NAME, outFilename)
            put(MediaStore.Downloads.MIME_TYPE, "application/octet-stream")
            put(MediaStore.Downloads.IS_PENDING, 1)
        }

        val resolver = context.contentResolver
        return resolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, contentValues)
    }

    @RequiresApi(Build.VERSION_CODES.Q)
    fun insertDownload(context: Context, outUri: Uri?, inputUri: Uri): Boolean {
        if (outUri == null) return false

        try {
            val resolver = context.contentResolver
            resolver.openInputStream(inputUri)?.use { inputStream ->
                resolver.openOutputStream(outUri)?.use { outputStream ->
                    inputStream.copyTo(outputStream)
                }
            }
            val contentValues = ContentValues().apply {
                put(MediaStore.Downloads.IS_PENDING, 0)
            }
            resolver.update(outUri, contentValues, null, null)

            return true
        } catch (_: FileNotFoundException) {
            return false
        }
    }

    fun File.getUri(context: Context): Uri {
        val authority = "${context.packageName}.fileprovider"
        return FileProvider.getUriForFile(context, authority, this)
    }

}
