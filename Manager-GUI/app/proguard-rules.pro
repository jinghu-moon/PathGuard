-keep class com.folder.manager.gui.XposedInit { *; }

# libsu
-keep class com.topjohnwu.superuser.** { *; }

# Kotlin 序列化/反射
-keepattributes *Annotation*
-keepattributes RuntimeVisibleAnnotations
-keepattributes Signature
-dontwarn kotlin.**
-dontwarn kotlinx.**
