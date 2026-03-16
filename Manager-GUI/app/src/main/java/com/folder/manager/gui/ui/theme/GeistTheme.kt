package com.folder.manager.gui.ui.theme

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

// ---------------------------------------------------------------------------
// Geist Design System — Color Palette
// ---------------------------------------------------------------------------

private object GeistColors {
    // Blue
    val Blue700 = Color(0xFF0072F5)
    val Blue600 = Color(0xFF419BF9)
    val Blue300 = Color(0xFFD8EDFE)
    val Blue1000 = Color(0xFF00254D)

    // Gray (Light)
    val Gray100L = Color(0xFFFFFFFF)
    val Gray200L = Color(0xFFFAFAFA)
    val Gray300L = Color(0xFFF5F5F5)
    val Gray400L = Color(0xFFEBEBEB)
    val Gray500L = Color(0xFFE1E1E1)
    val Gray600L = Color(0xFFC8C8C8)
    val Gray800L = Color(0xFF888888)
    val Gray900L = Color(0xFF6F6F6F)
    val Gray1000L = Color(0xFF171717)

    // Gray (Dark)
    val Gray100D = Color(0xFF1A1A1A)
    val Gray200D = Color(0xFF1F1F1F)
    val Gray300D = Color(0xFF292929)
    val Gray400D = Color(0xFF2E2E2E)
    val Gray500D = Color(0xFF454545)
    val Gray600D = Color(0xFF878787)
    val Gray1000D = Color(0xFFEDEDED)

    // Background
    val Bg100L = Color(0xFFFFFFFF)
    val Bg200L = Color(0xFFFAFAFA)
    val Bg100D = Color(0xFF0A0A0A)
    val Bg200D = Color(0xFF111111)

    // Error
    val ErrorL = Color(0xFFEE0000)
    val ErrorD = Color(0xFFFF0000)
    val ErrorContainerL = Color(0xFFFFF0F0)
    val ErrorContainerD = Color(0xFF3C0F0F)
}
private val LightColorScheme = lightColorScheme(
    primary            = GeistColors.Blue700,
    onPrimary          = GeistColors.Gray100L,
    primaryContainer   = GeistColors.Blue300,
    onPrimaryContainer = GeistColors.Blue1000,
    background         = GeistColors.Bg100L,
    onBackground       = GeistColors.Gray1000L,
    surface            = GeistColors.Bg200L,
    onSurface          = GeistColors.Gray1000L,
    surfaceVariant     = GeistColors.Gray300L,
    onSurfaceVariant   = GeistColors.Gray900L,
    outline            = GeistColors.Gray600L,
    outlineVariant     = GeistColors.Gray400L,
    error              = GeistColors.ErrorL,
    onError            = GeistColors.Gray100L,
    errorContainer     = GeistColors.ErrorContainerL,
    onErrorContainer   = GeistColors.ErrorL,
    secondaryContainer = GeistColors.Gray300L,
    onSecondaryContainer = GeistColors.Gray1000L,
)

private val DarkColorScheme = darkColorScheme(
    primary            = GeistColors.Blue600,
    onPrimary          = GeistColors.Blue1000,
    primaryContainer   = Color(0xFF006ADC),
    onPrimaryContainer = GeistColors.Blue300,
    background         = GeistColors.Bg100D,
    onBackground       = GeistColors.Gray1000D,
    surface            = GeistColors.Bg200D,
    onSurface          = GeistColors.Gray1000D,
    surfaceVariant     = GeistColors.Gray300D,
    onSurfaceVariant   = GeistColors.Gray600D,
    outline            = GeistColors.Gray500D,
    outlineVariant     = GeistColors.Gray400D,
    error              = GeistColors.ErrorD,
    onError            = GeistColors.Gray100D,
    errorContainer     = Color(0xFF3C0F0F),
    onErrorContainer   = Color(0xFFFF8080),
    secondaryContainer = GeistColors.Gray300D,
    onSecondaryContainer = GeistColors.Gray1000D,
)

@Composable
fun GeistTheme(
    darkTheme: Boolean = isSystemInDarkTheme(),
    content: @Composable () -> Unit,
) {
    MaterialTheme(
        colorScheme = if (darkTheme) DarkColorScheme else LightColorScheme,
        content = content,
    )
}
