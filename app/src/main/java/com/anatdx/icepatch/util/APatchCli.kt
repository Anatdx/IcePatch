package com.anatdx.icepatch.util

import android.content.ContentResolver
import android.content.Context
import android.content.pm.PackageManager
import android.content.pm.Signature
import android.database.Cursor
import android.net.Uri
import android.os.Build
import android.provider.OpenableColumns
import android.util.Base64
import android.util.Log
import com.topjohnwu.superuser.CallbackList
import com.topjohnwu.superuser.Shell
import com.topjohnwu.superuser.ShellUtils
import com.topjohnwu.superuser.internal.MainShell
import com.topjohnwu.superuser.io.SuFile
import com.anatdx.icepatch.APApplication
import com.anatdx.icepatch.APApplication.Companion.SUPERCMD
import com.anatdx.icepatch.BuildConfig
import com.anatdx.icepatch.Natives
import com.anatdx.icepatch.apApp
import com.anatdx.icepatch.ui.screen.MODULE_TYPE
import java.io.BufferedReader
import java.io.File
import java.io.InputStreamReader
import java.security.MessageDigest
import java.security.cert.CertificateFactory
import java.security.cert.X509Certificate
import java.util.Properties
import java.util.zip.ZipFile

private const val TAG = "APatchCli"

data class LocalApdResult(
    val code: Int,
    val lines: List<String>,
) {
    val isSuccess: Boolean
        get() = code == 0
}

class RootShellInitializer : Shell.Initializer() {
    override fun onInit(context: Context, shell: Shell): Boolean {
        shell.newJob().add("export PATH=\$PATH:/system_ext/bin:/vendor/bin").exec()
        return true
    }
}

private fun rootShellCommandFallbacks(globalMnt: Boolean = false): List<Array<String>> {
    val commands = mutableListOf<Array<String>>()
    if (APApplication.superKey.isNotEmpty()) {
        commands += arrayOf(
            SUPERCMD,
            APApplication.superKey,
            "-Z",
            APApplication.MAGISK_SCONTEXT,
        )
        // Some devices only accept default app context with supercmd.
        commands += arrayOf(
            SUPERCMD,
            APApplication.superKey,
            "-Z",
            APApplication.DEFAULT_SCONTEXT,
        )
        // Some devices/policies reject explicit -Z context; keep a no-context fallback.
        commands += arrayOf(SUPERCMD, APApplication.superKey)
    }
    val suPath = runCatching { Natives.suPath().trim() }.getOrDefault("")
    if (suPath.isNotEmpty() && suPath != "su") {
        commands += arrayOf(suPath)
    }
    if (suPath != "/system/bin/kp") {
        commands += arrayOf("/system/bin/kp")
    }
    if (globalMnt) {
        commands += arrayOf("su", "-mm")
    }
    commands += arrayOf("su")
    commands += arrayOf("sh")
    return commands
}

private fun shellCanAccessAdbDir(shell: Shell): Boolean {
    return ShellUtils.fastCmdResult(shell, "test -d /data/adb && ls /data/adb >/dev/null 2>&1")
}

private fun isUsableRootShell(shell: Shell, requireDataAdbAccess: Boolean): Boolean {
    if (!shell.isRoot) {
        return false
    }
    if (!requireDataAdbAccess) {
        return true
    }
    return shellCanAccessAdbDir(shell)
}

private fun shellQuote(value: String): String {
    return "'${value.replace("'", "'\"'\"'")}'"
}

@JvmOverloads
fun createRootShell(globalMnt: Boolean = false, requireDataAdbAccess: Boolean = false): Shell {
    Shell.enableVerboseLogging = BuildConfig.DEBUG
    val builder = Shell.Builder.create()
        .setInitializers(RootShellInitializer::class.java)
    for (cmd in rootShellCommandFallbacks(globalMnt)) {
        try {
            val shell = builder.build(*cmd)
            if (isUsableRootShell(shell, requireDataAdbAccess)) {
                return shell
            }
            Log.w(
                TAG,
                "skip unusable shell (${cmd.joinToString(" ")}), " +
                    "isRoot=${shell.isRoot}, requireDataAdbAccess=$requireDataAdbAccess"
            )
            shell.close()
        } catch (e: Throwable) {
            Log.e(TAG, "shell command failed (${cmd.joinToString(" ")}): ", e)
        }
    }
    return builder.build("sh")
}

private fun createMainRootShell() : Shell {
    val commands = rootShellCommandFallbacks(globalMnt = false)
    for (cmd in commands) {
        val builder = Shell.Builder.create()
            .setInitializers(RootShellInitializer::class.java)
            .setCommands(*cmd)
        try {
            val shell = builder.build()
            if (!isUsableRootShell(shell, requireDataAdbAccess = true)) {
                Log.w(
                    TAG,
                    "skip unusable main shell (${cmd.joinToString(" ")}), " +
                        "isRoot=${shell.isRoot}"
                )
                shell.close()
                continue
            }
            MainShell.setBuilder(builder)
            return shell
        } catch (e: Throwable) {
            Log.e(TAG, "main shell command failed (${cmd.joinToString(" ")}): ", e)
        }
    }
    val fallbackBuilder = Shell.Builder.create()
        .setInitializers(RootShellInitializer::class.java)
        .setCommands("sh")
    val shell = fallbackBuilder.build()
    MainShell.setBuilder(fallbackBuilder)
    return shell
}

object APatchCli {
    var SHELL: Shell = createMainRootShell()
    val GLOBAL_MNT_SHELL: Shell = createRootShell(true)
    fun refresh() {
        val tmp = SHELL

        val clazz = MainShell::class.java // reset MainShell
        clazz.getDeclaredField("isInitMain").apply {
            isAccessible = true
            setBoolean(null, false)
            isAccessible = false
        }

        clazz.getDeclaredField("mainShell").apply {
            isAccessible = true
            @Suppress("UNCHECKED_CAST")
            val arr = get(null) as Array<Any?>
            arr[0] = null
            isAccessible = false
        }

        clazz.getDeclaredField("mainBuilder").apply {
            isAccessible = true
            set(null, null)
            isAccessible = false
        }

        SHELL = createMainRootShell()
        tmp.close()
    }
}

fun getRootShell(globalMnt: Boolean = false): Shell {
    return if (globalMnt) {
        APatchCli.GLOBAL_MNT_SHELL
    } else {
        if (!APatchCli.SHELL.isRoot && APApplication.superKey.isNotEmpty()) {
            APatchCli.refresh()
        }
        APatchCli.SHELL
    }
}

inline fun <T> withNewRootShell(
    globalMnt: Boolean = false,
    requireDataAdbAccess: Boolean = false,
    block: Shell.() -> T
): T {
    return createRootShell(globalMnt, requireDataAdbAccess).use(block)
}

fun rootAvailable(): Boolean {
    val shell = getRootShell()
    return shell.isRoot
}

fun tryGetRootShell(): Shell {
    Shell.enableVerboseLogging = BuildConfig.DEBUG
    val builder = Shell.Builder.create()
    return try {
        builder.build(
            SUPERCMD, APApplication.superKey, "-Z", APApplication.MAGISK_SCONTEXT
        )
    } catch (e: Throwable) {
        Log.e(TAG, "su failed: ", e)
        return try {
            Log.e(TAG, "retry su: ", e)
            builder.build("su")
        } catch (e: Throwable) {
            Log.e(TAG, "retry su failed: ", e)
            builder.build("sh")
        }
    }
}

fun shellForResult(shell: Shell, vararg cmds: String): Shell.Result {
    val out = ArrayList<String>()
    val err = ArrayList<String>()
    return shell.newJob().add(*cmds).to(out, err).exec()
}

fun rootShellForResult(vararg cmds: String): Shell.Result {
    val out = ArrayList<String>()
    val err = ArrayList<String>()
    return getRootShell().newJob().add(*cmds).to(out, err).exec()
}

fun getLocalApdPath(): String? {
    val apd = File(apApp.applicationInfo.nativeLibraryDir, "libapd.so")
    return apd.takeIf { it.exists() }?.path
}

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

fun runLocalApd(
    vararg args: String,
    withSuperKey: Boolean = true,
    workDir: File = apApp.filesDir
): LocalApdResult {
    val apdPath = getLocalApdPath() ?: return LocalApdResult(-1, listOf("local apd missing"))
    val command = mutableListOf(apdPath)
    if (withSuperKey && APApplication.superKey.isNotBlank()) {
        command += listOf("--superkey", APApplication.superKey)
    }
    command.addAll(args)

    return runCatching {
        val builder = ProcessBuilder(command)
        builder.environment()["ASH_STANDALONE"] = "1"
        builder.directory(workDir)
        builder.redirectErrorStream(true)
        val process = builder.start()
        val lines = mutableListOf<String>()
        BufferedReader(InputStreamReader(process.inputStream)).use { reader ->
            var line: String?
            while (reader.readLine().also { line = it } != null) {
                lines += (line ?: "")
            }
        }
        LocalApdResult(process.waitFor(), lines)
    }.getOrElse {
        Log.e(TAG, "runLocalApd failed (${command.joinToString(" ")})", it)
        LocalApdResult(-1, listOf(it.message ?: "runLocalApd failed"))
    }
}

private fun runLocalApdInRootShell(
    vararg args: String,
    requireDataAdbAccess: Boolean,
    withSuperKey: Boolean = true,
    workDir: File = apApp.filesDir,
): LocalApdResult {
    val apdPath = getLocalApdPath() ?: return LocalApdResult(-1, listOf("local apd missing"))
    return runCatching {
        createRootShell(requireDataAdbAccess = requireDataAdbAccess).use { shell ->
            if (!shell.isRoot) {
                return@runCatching LocalApdResult(-1, listOf("no usable root shell"))
            }
            val command = buildList {
                add(apdPath)
                if (withSuperKey && APApplication.superKey.isNotBlank()) {
                    add("--superkey")
                    add(APApplication.superKey)
                }
                addAll(args)
            }.joinToString(" ") { shellQuote(it) }
            val out = ArrayList<String>()
            val err = ArrayList<String>()
            val result = shell.newJob()
                .add("cd ${shellQuote(workDir.absolutePath)} && ASH_STANDALONE=1 $command")
                .to(out, err)
                .exec()
            LocalApdResult(result.code, out + err)
        }
    }.getOrElse {
        Log.e(TAG, "runLocalApdInRootShell failed", it)
        LocalApdResult(-1, listOf(it.message ?: "runLocalApdInRootShell failed"))
    }
}

private fun localApdNeedsAdbDir(vararg args: String): Boolean {
    if (args.size < 2 || args[0] != "tool") {
        return false
    }
    return when (args[1]) {
        "ap", "package-config", "su-path" -> true
        else -> false
    }
}

fun runManagedApd(
    vararg args: String,
    withSuperKey: Boolean = true,
    workDir: File = apApp.filesDir,
): LocalApdResult {
    val requireDataAdbAccess = localApdNeedsAdbDir(*args)
    if (requireDataAdbAccess) {
        val rooted = runLocalApdInRootShell(
            *args,
            requireDataAdbAccess = true,
            withSuperKey = withSuperKey,
            workDir = workDir,
        )
        if (rooted.isSuccess) {
            return rooted
        }
        val direct = runLocalApd(*args, withSuperKey = withSuperKey, workDir = workDir)
        return if (direct.isSuccess) direct else rooted
    }

    val direct = runLocalApd(*args, withSuperKey = withSuperKey, workDir = workDir)
    if (direct.isSuccess) {
        return direct
    }
    return runLocalApdInRootShell(
        *args,
        requireDataAdbAccess = false,
        withSuperKey = withSuperKey,
        workDir = workDir,
    )
}

fun getLocalApdInfo(): Map<String, String> {
    val result = runManagedApd("tool", "ap", "status")
    if (!result.isSuccess) {
        Log.w(TAG, "getLocalApdInfo failed: ${result.lines.joinToString(" | ")}")
        return emptyMap()
    }
    return parseToolKv(result.lines)
}

fun installLocalApd(managerVersion: Long): LocalApdResult {
    return runManagedApd("tool", "ap", "install", "--manager-version", managerVersion.toString())
}

fun uninstallLocalApd(): LocalApdResult {
    return runManagedApd("tool", "ap", "uninstall")
}

fun readPersistedSuPath(): String? {
    val result = runManagedApd("tool", "su-path", "get")
    if (!result.isSuccess) {
        Log.w(TAG, "readPersistedSuPath failed: ${result.lines.joinToString(" | ")}")
        return null
    }
    return parseToolKv(result.lines)["su_path"]?.takeIf { it.isNotBlank() }
}

fun writePersistedSuPath(path: String): Boolean {
    val result = runManagedApd("tool", "su-path", "set", "--path", path)
    if (!result.isSuccess) {
        Log.w(TAG, "writePersistedSuPath failed: ${result.lines.joinToString(" | ")}")
    }
    return result.isSuccess
}

fun execApd(args: String, newShell: Boolean = false): Boolean {
    return if (newShell) {
        withNewRootShell {
            ShellUtils.fastCmdResult(this, "${APApplication.APD_PATH} $args")
        }
    } else {
        ShellUtils.fastCmdResult(getRootShell(), "${APApplication.APD_PATH} $args")
    }
}

fun listModules(): String {
    val shell = getRootShell()
    val out =
        shell.newJob().add("${APApplication.APD_PATH} module list").to(ArrayList(), null).exec().out
    withNewRootShell{
       newJob().add("cp /data/user/*/com.anatdx.icepatch/patch/ori.img /data/adb/ap/ && rm /data/user/*/com.anatdx.icepatch/patch/ori.img")
       .to(ArrayList(),null).exec()
   }
    return out.joinToString("\n").ifBlank { "[]" }
}

fun hasMetaModule(): Boolean {
    return getMetaModuleImplement() != "None"
}

fun getMetaModuleImplement(): String {
    try {
        val metaModuleProp = SuFile.open("/data/adb/metamodule/module.prop")
        if (!metaModuleProp.isFile) {
            Log.i(TAG, "Meta module implement: None")
            return "None"
        }

        val prop = Properties()
        prop.load(metaModuleProp.newInputStream())

        val name = prop.getProperty("name")
        Log.i(TAG, "Meta module implement: $name")
        return name
    } catch (t : Throwable) {
        Log.i(TAG, "Meta module implement: None")
        return "None"
    }
}

fun toggleModule(id: String, enable: Boolean): Boolean {
    val cmd = if (enable) {
        "module enable $id"
    } else {
        "module disable $id"
    }
    val result = execApd(cmd,true)
    Log.i(TAG, "$cmd result: $result")
    return result
}

fun uninstallModule(id: String): Boolean {
    val cmd = "module uninstall $id"
    val result = execApd(cmd,true)
    Log.i(TAG, "uninstall module $id result: $result")
    return result
}

fun installModule(
    uri: Uri, type: MODULE_TYPE, onFinish: (Boolean) -> Unit, onStdout: (String) -> Unit, onStderr: (String) -> Unit
): Boolean {
    val resolver = apApp.contentResolver
    with(resolver.openInputStream(uri)) {
        val fileExt = if (type == MODULE_TYPE.KPM) "kpm" else "zip"
        val file = File(apApp.cacheDir, "module_$type.$fileExt")
        file.outputStream().use { output ->
            this?.copyTo(output)
        }

        val stdoutCallback: CallbackList<String?> = object : CallbackList<String?>() {
            override fun onAddElement(s: String?) {
                onStdout(s ?: "")
            }
        }

        val stderrCallback: CallbackList<String?> = object : CallbackList<String?>() {
            override fun onAddElement(s: String?) {
                onStderr(s ?: "")
            }
        }

        val shell = getRootShell()

        var result = false
        if(type == MODULE_TYPE.APM) {
            val cmd = "${APApplication.APD_PATH} module install ${file.absolutePath}"
            result = shell.newJob().add(cmd).to(stdoutCallback, stderrCallback)
                    .exec().isSuccess
        } else {
            val rc = Natives.loadKernelPatchModule(file.absolutePath, "")
            onStdout("load kpm rc: $rc")
            result = rc == 0L
        }

        Log.i(TAG, "install $type module $uri result: $result")

        file.delete()

        onFinish(result)
        return result
    }
}

fun runAPModuleAction(
    moduleId: String, onStdout: (String) -> Unit, onStderr: (String) -> Unit
): Boolean {
    val stdoutCallback: CallbackList<String?> = object : CallbackList<String?>() {
        override fun onAddElement(s: String?) {
            onStdout(s ?: "")
        }
    }

    val stderrCallback: CallbackList<String?> = object : CallbackList<String?>() {
        override fun onAddElement(s: String?) {
            onStderr(s ?: "")
        }
    }

    val result = withNewRootShell{ 
        newJob().add("${APApplication.APD_PATH} module action $moduleId")
        .to(stdoutCallback, stderrCallback).exec()
    }
    Log.i(TAG, "APModule runAction result: $result")

    return result.isSuccess
}

fun reboot(reason: String = "") {
    if (reason == "recovery") {
        // KEYCODE_POWER = 26, hide incorrect "Factory data reset" message
        getRootShell().newJob().add("/system/bin/input keyevent 26").exec()
    }
    getRootShell().newJob()
        .add("/system/bin/svc power reboot $reason || /system/bin/reboot $reason").exec()
}

fun hasMagisk(): Boolean {
    val shell = getRootShell()
    val result = shell.newJob().add("nsenter --mount=/proc/1/ns/mnt which magisk").exec()
    Log.i(TAG, "has magisk: ${result.isSuccess}")
    return result.isSuccess
}

fun isGlobalNamespaceEnabled(): Boolean {
    val shell = getRootShell()
    val result = ShellUtils.fastCmd(shell, "cat ${APApplication.GLOBAL_NAMESPACE_FILE}")
    Log.i(TAG, "is global namespace enabled: $result")
    return result == "1"
}

fun setGlobalNamespaceEnabled(value: String) {
    getRootShell().newJob().add("echo $value > ${APApplication.GLOBAL_NAMESPACE_FILE}")
        .submit { result ->
            Log.i(TAG, "setGlobalNamespaceEnabled result: ${result.isSuccess} [${result.out}]")
        }
}

fun getFileNameFromUri(context: Context, uri: Uri): String? {
    var fileName: String? = null
    val contentResolver: ContentResolver = context.contentResolver
    val cursor: Cursor? = contentResolver.query(uri, null, null, null, null)
    cursor?.use {
        if (it.moveToFirst()) {
            fileName = it.getString(it.getColumnIndexOrThrow(OpenableColumns.DISPLAY_NAME))
        }
    }
    return fileName
}

@Suppress("DEPRECATION")
private fun signatureFromAPI(context: Context): ByteArray? {
    return try {
        val packageInfo = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            context.packageManager.getPackageInfo(
                context.packageName, PackageManager.GET_SIGNING_CERTIFICATES
            )
        } else {
            context.packageManager.getPackageInfo(
                context.packageName,
                PackageManager.GET_SIGNATURES
            )
        }

        val signatures: Array<out Signature>? =
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                packageInfo.signingInfo?.apkContentsSigners
            } else {
                packageInfo.signatures
            }

        signatures?.firstOrNull()?.toByteArray()
    } catch (e: Exception) {
        e.printStackTrace()
        null
    }
}

private fun signatureFromAPK(context: Context): ByteArray? {
    var signatureBytes: ByteArray? = null
    try {
        ZipFile(context.packageResourcePath).use { zipFile ->
            val entries = zipFile.entries()
            while (entries.hasMoreElements() && signatureBytes == null) {
                val entry = entries.nextElement()
                if (entry.name.matches("(META-INF/.*)\\.(RSA|DSA|EC)".toRegex())) {
                    zipFile.getInputStream(entry).use { inputStream ->
                        val certFactory = CertificateFactory.getInstance("X509")
                        val x509Cert =
                            certFactory.generateCertificate(inputStream) as X509Certificate
                        signatureBytes = x509Cert.encoded
                    }
                }
            }
        }
    } catch (e: Exception) {
        e.printStackTrace()
    }
    return signatureBytes
}

private fun validateSignature(signatureBytes: ByteArray?, validSignature: String): Boolean {
    signatureBytes ?: return false
    val digest = MessageDigest.getInstance("SHA-256")
    val signatureHash = Base64.encodeToString(digest.digest(signatureBytes), Base64.NO_WRAP)
    return signatureHash == validSignature
}

fun verifyAppSignature(validSignature: String): Boolean {
    val context = apApp.applicationContext
    val apkSignature = signatureFromAPK(context)
    val apiSignature = signatureFromAPI(context)

    return validateSignature(apiSignature, validSignature) || validateSignature(
        apkSignature,
        validSignature
    )
}
