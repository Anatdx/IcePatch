@file:Suppress("UnstableApiUsage")

import com.android.build.gradle.tasks.PackageAndroidArtifact
import org.jetbrains.kotlin.gradle.dsl.JvmTarget
import java.net.URI

plugins {
    alias(libs.plugins.agp.app)
    alias(libs.plugins.kotlin.compose.compiler)
    alias(libs.plugins.ksp)
    alias(libs.plugins.lsplugin.apksign)
    alias(libs.plugins.lsplugin.resopt)
    id("kotlin-parcelize")
}

val androidCompileSdkVersion: Int by rootProject.extra
val androidCompileNdkVersion: String by rootProject.extra
val androidBuildToolsVersion: String by rootProject.extra
val androidMinSdkVersion: Int by rootProject.extra
val androidTargetSdkVersion: Int by rootProject.extra
val androidSourceCompatibility: JavaVersion by rootProject.extra
val androidTargetCompatibility: JavaVersion by rootProject.extra
val managerVersionCode: Int by rootProject.extra
val managerVersionName: String by rootProject.extra
val branchName: String by rootProject.extra
val kernelPatchVersion: String by rootProject.extra

apksign {
    storeFileProperty = "KEYSTORE_FILE"
    storePasswordProperty = "KEYSTORE_PASSWORD"
    keyAliasProperty = "KEY_ALIAS"
    keyPasswordProperty = "KEY_PASSWORD"
}

val ccache = System.getenv("PATH")?.split(File.pathSeparator)
    ?.map { File(it, "ccache") }?.firstOrNull { it.exists() }?.absolutePath

val baseFlags = listOf(
    "-Wall", "-Qunused-arguments", "-fno-rtti", "-fvisibility=hidden",
    "-fvisibility-inlines-hidden", "-fno-exceptions", "-fno-stack-protector",
    "-fomit-frame-pointer", "-Wno-builtin-macro-redefined", "-Wno-unused-value",
    "-D__FILE__=__FILE_NAME__",
    "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON", "-Wno-unused", "-Wno-unused-parameter",
    "-Wno-unused-command-line-argument", "-Wno-incompatible-function-pointer-types",
    "-U_FORTIFY_SOURCE", "-D_FORTIFY_SOURCE=0"
)

val baseArgs = mutableListOf(
    "-DANDROID_STL=none", "-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON",
    "-DCMAKE_CXX_STANDARD=23", "-DCMAKE_C_STANDARD=23",
    "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON", "-DCMAKE_VISIBILITY_INLINES_HIDDEN=ON",
    "-DCMAKE_CXX_VISIBILITY_PRESET=hidden", "-DCMAKE_C_VISIBILITY_PRESET=hidden"
).apply { if (ccache != null) add("-DANDROID_CCACHE=$ccache") }

val keystoreFileValue = providers.gradleProperty("KEYSTORE_FILE").orNull
    ?: System.getenv("KEYSTORE_FILE")
val keystorePasswordValue = providers.gradleProperty("KEYSTORE_PASSWORD").orNull
    ?: System.getenv("KEYSTORE_PASSWORD")
val keyAliasValue = providers.gradleProperty("KEY_ALIAS").orNull
    ?: System.getenv("KEY_ALIAS")
val keyPasswordValue = providers.gradleProperty("KEY_PASSWORD").orNull
    ?: System.getenv("KEY_PASSWORD")
val hasReleaseSigning =
    !keystoreFileValue.isNullOrBlank() &&
        !keystorePasswordValue.isNullOrBlank() &&
        !keyAliasValue.isNullOrBlank() &&
        !keyPasswordValue.isNullOrBlank()

android {
    namespace = "com.anatdx.icepatch"

    signingConfigs {
        create("release") {
            if (hasReleaseSigning) {
                storeFile = file(keystoreFileValue!!)
                storePassword = keystorePasswordValue
                keyAlias = keyAliasValue
                keyPassword = keyPasswordValue
                enableV1Signing = true
                enableV2Signing = true
            } else {
                logger.lifecycle("Release signing disabled: set KEYSTORE_* to enable.")
            }
        }
    }

    buildTypes {
        debug {
            isDebuggable = true
            isMinifyEnabled = false
            isShrinkResources = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            externalNativeBuild {
                cmake {
                    arguments += listOf("-DCMAKE_CXX_FLAGS_DEBUG=-Og", "-DCMAKE_C_FLAGS_DEBUG=-Og")
                }
            }
        }
        release {
            isMinifyEnabled = true
            isShrinkResources = true
            isDebuggable = false
            multiDexEnabled = true
            if (hasReleaseSigning) {
                signingConfig = signingConfigs.getByName("release")
            }
            vcsInfo.include = false
            proguardFiles(
                getDefaultProguardFile("proguard-android-optimize.txt"),
                "proguard-rules.pro"
            )
            externalNativeBuild {
                cmake {
                    val relFlags = listOf(
                        "-flto", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                        "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-Wl,--exclude-libs,ALL",
                        "-Ofast", "-fmerge-all-constants", "-flto=full", "-ffat-lto-objects",
                        "-fno-semantic-interposition", "-fno-threadsafe-statics"
                    )
                    cppFlags += relFlags
                    cFlags += relFlags
                    arguments += listOf("-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG", "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG")
                }
            }
        }
    }

    dependenciesInfo.includeInApk = false

    buildFeatures {
        aidl = true
        buildConfig = true
        compose = true
        prefab = true
    }

    defaultConfig {
        minSdk = androidMinSdkVersion
        targetSdk = androidTargetSdkVersion
        versionCode = managerVersionCode
        versionName = managerVersionName
        ndk.abiFilters.addAll(arrayOf("arm64-v8a"))
        externalNativeBuild {
            cmake {
                cppFlags += baseFlags + "-std=c++2b"
                cFlags += baseFlags + "-std=c2x"
                arguments += baseArgs
                abiFilters("arm64-v8a")
            }
        }
        buildConfigField("String", "buildKPV", "\"$kernelPatchVersion\"")
        base.archivesName = "IcePatch_${managerVersionCode}_${managerVersionName}_${branchName}"
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_21
        targetCompatibility = JavaVersion.VERSION_21
    }

    packaging {
        jniLibs {
            useLegacyPackaging = true
        }
        resources {
            excludes += "**"
            merges += "META-INF/com/google/android/**"
        }
    }

    externalNativeBuild {
        cmake {
            version = "3.28.0+"
            path("src/main/cpp/CMakeLists.txt")
        }
    }

    androidResources {
        generateLocaleConfig = true
    }

    compileSdk = androidCompileSdkVersion
    ndkVersion = androidCompileNdkVersion
    buildToolsVersion = androidBuildToolsVersion

    lint {
        abortOnError = false
        checkReleaseBuilds = false
    }

    android.sourceSets.named("main") {
        kotlin.directories += "build/generated/ksp/$name/kotlin"
        jniLibs.directories += "libs"
    }
}

// https://stackoverflow.com/a/77745844
tasks.withType<PackageAndroidArtifact> {
    doFirst { appMetadata.asFile.orNull?.writeText("") }
}

java {
    toolchain {
        languageVersion = JavaLanguageVersion.of(21)
    }
}

kotlin {
    jvmToolchain(21)
    compilerOptions {
        jvmTarget = JvmTarget.JVM_21
    }
}

fun registerDownloadTask(
    taskName: String, srcUrl: String, destPath: String, project: Project
) {
    project.tasks.register(taskName) {
        val destFile = File(destPath)

        doLast {
            if (!destFile.exists() || isFileUpdated(srcUrl, destFile)) {
                println(" - Downloading $srcUrl to ${destFile.absolutePath}")
                downloadFile(srcUrl, destFile)
                println(" - Download completed.")
            } else {
                println(" - File is up-to-date, skipping download.")
            }
        }
    }
}

fun isFileUpdated(url: String, localFile: File): Boolean {
    val connection = URI.create(url).toURL().openConnection()
    val remoteLastModified = connection.getHeaderFieldDate("Last-Modified", 0L)
    return remoteLastModified > localFile.lastModified()
}

fun downloadFile(url: String, destFile: File) {
    URI.create(url).toURL().openStream().use { input ->
        destFile.outputStream().use { output ->
            input.copyTo(output)
        }
    }
}

registerDownloadTask(
    taskName = "downloadKpimg",
    srcUrl = "https://github.com/bmax121/KernelPatch/releases/download/$kernelPatchVersion/kpimg-android",
    destPath = "${project.projectDir}/src/main/assets/kpimg",
    project = project
)

registerDownloadTask(
    taskName = "downloadKptools",
    srcUrl = "https://github.com/bmax121/KernelPatch/releases/download/$kernelPatchVersion/kptools-android",
    destPath = "${project.projectDir}/libs/arm64-v8a/libkptools.so",
    project = project
)

// Compat kp version less than 0.10.7
// TODO: Remove in future
registerDownloadTask(
    taskName = "downloadCompatKpatch",
    srcUrl = "https://github.com/bmax121/KernelPatch/releases/download/0.10.7/kpatch-android",
    destPath = "${project.projectDir}/libs/arm64-v8a/libkpatch.so",
    project = project
)

tasks.register<Copy>("mergeScripts") {
    into("${project.projectDir}/src/main/resources/META-INF/com/google/android")
    from(rootProject.file("${project.rootDir}/scripts/update_binary.sh")) {
        rename { "update-binary" }
    }
    from(rootProject.file("${project.rootDir}/scripts/update_script.sh")) {
        rename { "updater-script" }
    }
}

tasks.getByName("preBuild").dependsOn(
    "downloadKpimg",
    "downloadKptools",
    "downloadCompatKpatch",
    "mergeScripts",
)

tasks.register<Exec>("cmakeConfigureApd") {
    val ndkDir = System.getenv("ANDROID_NDK_HOME")
        ?: System.getenv("ANDROID_NDK_ROOT")
        ?: throw GradleException("ANDROID_NDK_HOME/ANDROID_NDK_ROOT is not set.")
    commandLine(
        "cmake",
        "-S", "${project.rootDir}/apd",
        "-B", "${project.rootDir}/apd/build",
        "-DCMAKE_TOOLCHAIN_FILE=$ndkDir/build/cmake/android.toolchain.cmake",
        "-DANDROID_ABI=arm64-v8a",
        "-DANDROID_PLATFORM=android-21",
        "-DCMAKE_BUILD_TYPE=Release",
        "-DAPD_OUTPUT_DIR=${project.rootDir}/apd/build/output"
    )
}

tasks.register<Exec>("cmakeBuildApd") {
    dependsOn("cmakeConfigureApd")
    commandLine(
        "cmake",
        "--build", "${project.rootDir}/apd/build",
        "--parallel"
    )
}

tasks.register<Copy>("buildApd") {
    dependsOn("cmakeBuildApd")
    from("${project.rootDir}/apd/build/output/apd")
    into("${project.projectDir}/libs/arm64-v8a")
    rename("apd", "libapd.so")
}

tasks.configureEach {
    if (name == "mergeDebugJniLibFolders" || name == "mergeReleaseJniLibFolders") {
        dependsOn("buildApd")
    }
}

tasks.register<Delete>("apdClean") {
    delete(
        file("${project.projectDir}/libs/arm64-v8a/libapd.so"),
        file("${project.rootDir}/apd/build")
    )
}

tasks.clean {
    dependsOn("apdClean")
}

ksp {
    arg("compose-destinations.defaultTransitions", "none")
}

dependencies {
    implementation(libs.androidx.appcompat)
    implementation(libs.androidx.activity.compose)
    implementation(libs.androidx.core.splashscreen)
    implementation(libs.androidx.webkit)

    implementation(platform(libs.androidx.compose.bom))
    implementation(libs.androidx.compose.material.icons.extended)
    implementation(libs.androidx.compose.material3)
    implementation(libs.androidx.compose.ui)
    implementation(libs.androidx.compose.ui.tooling.preview)
    implementation(libs.androidx.compose.runtime.livedata)

    debugImplementation(libs.androidx.compose.ui.test.manifest)
    debugImplementation(libs.androidx.compose.ui.tooling)

    implementation(libs.androidx.lifecycle.runtime.compose)
    implementation(libs.androidx.lifecycle.runtime.ktx)
    implementation(libs.androidx.lifecycle.viewmodel.compose)

    implementation(libs.compose.destinations.core)
    ksp(libs.compose.destinations.ksp)

    implementation(libs.com.github.topjohnwu.libsu.core)
    implementation(libs.com.github.topjohnwu.libsu.service)
    implementation(libs.com.github.topjohnwu.libsu.nio)
    implementation(libs.com.github.topjohnwu.libsu.io)

    implementation(libs.dev.rikka.rikkax.parcelablelist)

    implementation(libs.io.coil.kt.coil.compose)

    implementation(libs.kotlinx.coroutines.core)

    implementation(libs.me.zhanghai.android.appiconloader.coil)

    implementation(libs.sheet.compose.dialogs.core)
    implementation(libs.sheet.compose.dialogs.list)
    implementation(libs.sheet.compose.dialogs.input)

    implementation(libs.markdown)

    implementation(libs.ini4j)

    compileOnly(libs.cxx)
}
