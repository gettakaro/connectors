import io.takaro.gradle.TargetCatalog

// ModDevGradle and shadow are applied here rather than from a convention plugin: putting
// them on buildSrc's classpath would break the targets that request them by version alias.
plugins {
    alias(libs.plugins.neoforge.moddev)
    alias(libs.plugins.shadow)
    id("takaro.neoforge-target")
}

val target = TargetCatalog.load(project)

neoForge {
    // The NeoForge version comes from the catalog record, never from the version catalog.
    version = target.input("loader")["loaderVersion"] as String

    mods {
        create("takaro") {
            sourceSet(sourceSets.main.get())
        }
    }
}

val shade: Configuration by configurations.getting

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
    exclude("org/slf4j/**")
    exclude("META-INF/maven/org.slf4j/**")
}
