package com.anatdx.icepatch.ui.viewmodel

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.content.pm.ApplicationInfo
import android.content.pm.PackageInfo
import android.graphics.drawable.Drawable
import android.os.IBinder
import android.os.Parcelable
import android.util.Log
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.ViewModel
import com.topjohnwu.superuser.Shell
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.coroutines.withContext
import kotlinx.parcelize.Parcelize
import com.anatdx.icepatch.APApplication
import com.anatdx.icepatch.IAPRootService
import com.anatdx.icepatch.Natives
import com.anatdx.icepatch.apApp
import com.anatdx.icepatch.services.RootServices
import com.anatdx.icepatch.util.APatchCli
import com.anatdx.icepatch.util.HanziToPinyin
import com.anatdx.icepatch.util.PkgConfig
import java.text.Collator
import java.util.Locale
import kotlin.coroutines.resume
import kotlin.coroutines.suspendCoroutine


class SuperUserViewModel : ViewModel() {
    companion object {
        private const val TAG = "SuperUserViewModel"
        private val appsLock = Any()
        var apps by mutableStateOf<List<AppInfo>>(emptyList())

        fun getAppIconDrawable(context: Context, packageName: String): Drawable? {
            val appList = synchronized(appsLock) { apps }
            val appDetail = appList.find { it.packageName == packageName }
            return appDetail?.packageInfo?.applicationInfo?.loadIcon(context.packageManager)
        }
    }

    @Parcelize
    data class AppInfo(
        val label: String, val packageInfo: PackageInfo, val config: PkgConfig.Config
    ) : Parcelable {
        val packageName: String
            get() = packageInfo.packageName
        val uid: Int
            get() = packageInfo.applicationInfo!!.uid
    }

    var search by mutableStateOf("")
    var showSystemApps by mutableStateOf(false)
    var isRefreshing by mutableStateOf(false)
        private set

    private val sortedList by derivedStateOf {
        val comparator = compareBy<AppInfo> {
            when {
                it.config.allow != 0 -> 0
                it.config.exclude == 1 -> 1
                else -> 2
            }
        }.then(compareBy(Collator.getInstance(Locale.getDefault()), AppInfo::label))
        apps.sortedWith(comparator)
    }

    val appList by derivedStateOf {
        sortedList.filter {
            it.label.lowercase().contains(search.lowercase()) || it.packageName.lowercase()
                .contains(search.lowercase()) || HanziToPinyin.getInstance()
                .toPinyinString(it.label).contains(search.lowercase())
        }.filter {
            it.uid == 2000 // Always show shell
                    || showSystemApps || it.packageInfo.applicationInfo!!.flags.and(ApplicationInfo.FLAG_SYSTEM) == 0
        }
    }

    private suspend inline fun connectRootService(
        crossinline onDisconnect: () -> Unit = {}
    ): Pair<IBinder, ServiceConnection> = suspendCoroutine {
        val connection = object : ServiceConnection {
            override fun onServiceDisconnected(name: ComponentName?) {
                onDisconnect()
            }

            override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
                it.resume(binder as IBinder to this)
            }
        }
        val intent = Intent(apApp, RootServices::class.java)
        val task = RootServices.bindOrTask(
            intent,
            Shell.EXECUTOR,
            connection,
        )
        val shell = APatchCli.SHELL
        task?.let { it1 -> shell.execTask(it1) }
    }

    private fun stopRootService() {
        val intent = Intent(apApp, RootServices::class.java)
        RootServices.stop(intent)
    }

    private fun buildAppList(
        packages: List<PackageInfo>,
        rootlessMode: Boolean
    ): List<AppInfo> {
        val uids = if (rootlessMode) {
            emptySet()
        } else {
            Natives.suUids().toSet()
        }
        val configs = if (rootlessMode) {
            hashMapOf()
        } else {
            PkgConfig.readConfigs()
        }

        return packages.map { packageInfo ->
            val appInfo = packageInfo.applicationInfo
            val uid = appInfo!!.uid
            val actProfile = if (!rootlessMode && uids.contains(uid)) Natives.suProfile(uid) else null
            val defaultConfig = PkgConfig.Config(
                appInfo.packageName,
                Natives.isUidExcluded(uid),
                0,
                Natives.Profile(uid = uid)
            )
            val config = if (rootlessMode) {
                defaultConfig
            } else {
                configs.getOrDefault(uid, defaultConfig)
            }
            config.allow = 0

            if (actProfile != null) {
                config.allow = 1
                config.profile = actProfile
            }
            AppInfo(
                label = appInfo.loadLabel(apApp.packageManager).toString(),
                packageInfo = packageInfo,
                config = config
            )
        }
    }

    suspend fun fetchAppList(forceRootlessMode: Boolean = APApplication.rootlessModeLiveData.value == true) {
        isRefreshing = true

        try {
            val rootlessMode = forceRootlessMode
            val allPackages = if (rootlessMode) {
                withContext(Dispatchers.IO) {
                    @Suppress("DEPRECATION")
                    apApp.packageManager.getInstalledPackages(0)
                }
            } else {
                val result = withTimeoutOrNull(3000L) {
                    connectRootService {
                        Log.w(TAG, "RootService disconnected")
                    }
                }
                if (result == null) {
                    Log.w(TAG, "RootService connect timed out, fallback to PackageManager")
                    withContext(Dispatchers.IO) {
                        @Suppress("DEPRECATION")
                        apApp.packageManager.getInstalledPackages(0)
                    }
                } else {
                    withContext(Dispatchers.IO) {
                        val binder = result.first
                        val packages = IAPRootService.Stub.asInterface(binder).getPackages(0).list
                        withContext(Dispatchers.Main) {
                            stopRootService()
                        }
                        packages
                    }
                }
            }

            val newApps = withContext(Dispatchers.IO) {
                buildAppList(allPackages, rootlessMode)
            }

            synchronized(appsLock) {
                apps = newApps
            }
        } catch (e: Exception) {
            Log.e(TAG, "Failed to fetch app list", e)
        } finally {
            isRefreshing = false
        }
    }
}
