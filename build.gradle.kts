plugins {
    alias(libs.plugins.agp.app) apply false
    alias(libs.plugins.kotlin) apply false
    alias(libs.plugins.kotlin.compose.compiler) apply false
}

fun readKernelPatchVersion(): String {
    val versionFile = file("external/KernelPatch/version")
    if (!versionFile.isFile) return "0.13.0"
    val raw = versionFile.readText()
    val major = Regex("""#define\s+MAJOR\s+(\d+)""").find(raw)?.groupValues?.get(1)
    val minor = Regex("""#define\s+MINOR\s+(\d+)""").find(raw)?.groupValues?.get(1)
    val patch = Regex("""#define\s+PATCH\s+(\d+)""").find(raw)?.groupValues?.get(1)
    if (major != null && minor != null && patch != null) {
        return "$major.$minor.$patch"
    }
    return raw.trim().ifEmpty { "0.13.0" }
}

project.ext.set("kernelPatchVersion", readKernelPatchVersion())

val androidMinSdkVersion by extra(26)
val androidTargetSdkVersion by extra(36)
val androidCompileSdkVersion by extra(36)
val androidBuildToolsVersion by extra("36.1.0")
val androidCompileNdkVersion by extra("29.0.14206865")
val managerVersionCode by extra(getVersionCode())
val managerVersionName by extra(getVersionName())
val branchName by extra(getBranch())
fun Project.exec(command: String) = providers.exec {
    commandLine(command.split(" "))
}.standardOutput.asText.get().trim()

fun getGitCommitCount(): Int {
    return exec("git rev-list --count HEAD").trim().toInt()
}

fun getGitDescribe(): String {
    return exec("git rev-parse --verify --short HEAD").trim()
}

fun getVersionCode(): Int {
    val commitCount = getGitCommitCount()
    val major = 1
    return major * 10000 + commitCount + 200
}

fun getBranch(): String {
    return exec("git rev-parse --abbrev-ref HEAD").trim()
}

fun getVersionName(): String {
    return getGitDescribe()
}

tasks.register("printVersion") {
    doLast {
        println("Version code: $managerVersionCode")
        println("Version name: $managerVersionName")
    }
}
