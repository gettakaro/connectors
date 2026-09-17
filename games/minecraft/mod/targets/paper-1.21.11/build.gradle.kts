// Shadow is applied here rather than from a convention plugin: putting it on buildSrc's
// classpath would break the targets that request it by version alias.
plugins {
    alias(libs.plugins.shadow)
    id("takaro.paper-target")
}

val shade: Configuration by configurations.getting

// The shipped jar is the shadow jar: the plugin compiles against the Paper API, so nothing
// needs remapping, and the shaded WebSocket and Gson copies are relocated so they cannot
// collide with another plugin's.
tasks.shadowJar {
    configurations = listOf(shade)
    archiveClassifier.set("")
    relocate("org.java_websocket", "io.takaro.libs.websocket")
    relocate("com.google.gson", "io.takaro.libs.gson")
    exclude("com/google/errorprone/**")
    exclude("com/google/j2objc/**")
    exclude("javax/annotation/**")
    exclude("org/checkerframework/**")
    exclude("META-INF/maven/com.google.errorprone/**")
    exclude("META-INF/maven/com.google.j2objc/**")
    exclude("META-INF/maven/org.checkerframework/**")
}
