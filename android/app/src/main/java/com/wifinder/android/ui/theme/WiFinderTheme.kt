package com.wifinder.android.ui.theme

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color

private val Teal = Color(0xFF00BFA5)
private val TealDark = Color(0xFF008C7A)
private val TealLight = Color(0xFF5DF2D6)

private val SurfaceDark = Color(0xFF121212)
private val SurfaceContainerDark = Color(0xFF1E1E1E)
private val SurfaceContainerHighDark = Color(0xFF2A2A2A)
private val OnSurfaceDark = Color(0xFFE0E0E0)
private val OnSurfaceVariantDark = Color(0xFF9E9E9E)

private val Amber = Color(0xFFFFB300)
private val ErrorRed = Color(0xFFCF6679)

private val DarkColorScheme = darkColorScheme(
  primary = Teal,
  onPrimary = Color.Black,
  primaryContainer = TealDark,
  onPrimaryContainer = TealLight,
  secondary = Amber,
  onSecondary = Color.Black,
  background = SurfaceDark,
  onBackground = OnSurfaceDark,
  surface = SurfaceContainerDark,
  onSurface = OnSurfaceDark,
  surfaceVariant = SurfaceContainerHighDark,
  onSurfaceVariant = OnSurfaceVariantDark,
  error = ErrorRed,
  onError = Color.Black,
  outline = Color(0xFF444444),
  outlineVariant = Color(0xFF333333),
)

private val LightColorScheme = lightColorScheme(
  primary = TealDark,
  onPrimary = Color.White,
  primaryContainer = Color(0xFFB2DFDB),
  onPrimaryContainer = Color(0xFF00382E),
  secondary = Amber,
  onSecondary = Color.Black,
)

@Composable
fun WiFinderTheme(
  darkTheme: Boolean = isSystemInDarkTheme(),
  content: @Composable () -> Unit,
) {
  MaterialTheme(
    colorScheme = if (darkTheme) DarkColorScheme else LightColorScheme,
    content = content,
  )
}
