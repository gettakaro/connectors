import io.takaro.gradle.TargetCatalog

// Loom and shadow are applied here rather than from a convention plugin: putting them on
// buildSrc's classpath would break the legacy modules that still request them by version.
plugins {
    alias(libs.plugins.fabric.loom)
    alias(libs.plugins.shadow)
    id("takaro.target-base")
}

val target = TargetCatalog.load(project)

repositories {
    maven("https://maven.fabricmc.net/")
}

// The Fabric sources are shared by every Fabric target; a target may overlay its own
// sources under targets/<id>/src/main/java when a game version needs different code.
sourceSets.named("main") {
    java.srcDir(layout.projectDirectory.dir("../../fabric/src/main/java"))
    resources.srcDir(layout.projectDirectory.dir("../../fabric/src/main/resources"))
}

val shade: Configuration by configurations.creating

dependencies {
    minecraft(target.coordinate("minecraft"))
    implementation(target.coordinate("fabric-loader"))
    implementation(target.coordinate("fabric-api"))

    shade(project(":core"))
    implementation(project(":core"))
}

// The shipped jar is the shadow jar: the connector compiles against the game's own names,
// so nothing needs remapping, and the shaded WebSocket and Gson copies are relocated so
// they cannot collide with another mod's.
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

    // A component's file name is part of its identity: `takaro-maint` looks the artifact up
    // by this exact name instead of globbing build/libs, so a rename fails here first.
    doLast {
        @Suppress("UNCHECKED_CAST")
        val components = target.data["components"] as List<Map<String, Any?>>
        val pattern = components.first { it["role"] == "server-mod" }["artifact"] as String
        val expected = pattern.replace("{version}", project.version.toString())
        val produced = archiveFile.get().asFile.name
        check(produced == expected) {
            "the Fabric target produced $produced but the catalog names its server-mod $expected"
        }
    }
}

tasks.build {
    dependsOn(tasks.shadowJar)
}

// Loom registers remapJar with the same archive name. This target ships the shadow jar,
// so the remap task would only race it for the same file.
afterEvaluate {
    tasks.findByName("remapJar")?.enabled = false
    tasks.findByName("remapSourcesJar")?.enabled = false
}
