package com.folder.manager.gui.ui

import android.content.Intent
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.lifecycle.lifecycleScope
import com.folder.manager.gui.MainActivity
import com.folder.manager.gui.R
import com.folder.manager.gui.data.NotificationHelper
import com.folder.manager.gui.ui.theme.GeistTheme
import com.topjohnwu.superuser.Shell
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * Launch screen: checks root permission, navigates to MainActivity on success.
 */
class WelcomeActivity : ComponentActivity() {

    private val notifPermLauncher = registerForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { /* 用户选择后继续，不强制要求 */ }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        // 初始化通知渠道
        NotificationHelper.createChannels(this)
        // Android 13+ 请求通知权限
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            notifPermLauncher.launch(android.Manifest.permission.POST_NOTIFICATIONS)
        }
        // Shell already ready (process reuse) — skip welcome screen
        if (Shell.isAppGrantedRoot() == true) {
            goMain()
            return
        }

        setContent {
            GeistTheme {
                WelcomeScreen(onGrant = { requestRoot() })
            }
        }
    }

    private fun requestRoot() {
        lifecycleScope.launch {
            val granted = withContext(Dispatchers.IO) {
                Shell.getShell() // triggers system root dialog
                Shell.isAppGrantedRoot() == true
            }
            if (granted) {
                goMain()
            } else {
                setContent {
                    GeistTheme {
                        WelcomeScreen(onGrant = { requestRoot() }, failed = true)
                    }
                }
            }
        }
    }

    private fun goMain() {
        startActivity(Intent(this, MainActivity::class.java))
        finish()
    }
}

@Composable
private fun WelcomeScreen(
    onGrant: () -> Unit,
    failed: Boolean = false,
) {
    var checking by remember { mutableStateOf(false) }

    Surface(
        modifier = Modifier.fillMaxSize(),
        color = MaterialTheme.colorScheme.background,
    ) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .statusBarsPadding()
                .navigationBarsPadding()
                .padding(horizontal = 32.dp),
            verticalArrangement = Arrangement.Center,
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text(
                text = stringResource(R.string.welcome_title),
                style = MaterialTheme.typography.headlineLarge,
            )

            Spacer(modifier = Modifier.height(16.dp))

            Text(
                text = if (failed)
                    stringResource(R.string.please_grant_root)
                else
                    stringResource(R.string.welcome_desc),
                style = MaterialTheme.typography.bodyMedium,
                textAlign = TextAlign.Center,
                color = if (failed)
                    MaterialTheme.colorScheme.error
                else
                    MaterialTheme.colorScheme.onSurfaceVariant,
            )

            Spacer(modifier = Modifier.height(32.dp))

            if (checking) {
                CircularProgressIndicator()
            } else {
                Button(
                    onClick = {
                        checking = true
                        onGrant()
                    },
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    Text(stringResource(R.string.welcome_grant))
                }
            }
        }
    }
}
