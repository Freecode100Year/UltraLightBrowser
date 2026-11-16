# JavaScript bridges are called by name from page scripts.
-keepclassmembers class * { @android.webkit.JavascriptInterface <methods>; }
