plugins {
    alias(libs.plugins.fabric.loom)
    alias(libs.plugins.shadow)
}

val shade: Configuration by configurations.creating

dependencies {
    minecraft("com.mojang:minecraft:${libs.versions.minecraft.get()}")
    mappings(loom.officialMojangMappings())
    modImplementation("net.fabricmc:fabric-loader:${libs.versions.fabric.loader.get()}")
    modImplementation("net.fabricmc.fabric-api:fabric-api:${libs.versions.fabric.api.get()}")

    shade(project(":core"))
    implementation(project(":core"))
}

tasks.shadowJar {
    configurations = listOf(shade)
    archiveClassifier.set("dev-shadow")
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

tasks.remapJar {
    inputFile.set(tasks.shadowJar.get().archiveFile)
    dependsOn(tasks.shadowJar)
}
