plugins {
    alias(libs.plugins.fabric.loom)
    alias(libs.plugins.shadow)
}

// Minecraft 26.2 requires Java 25; the other Minecraft modules stay on 21.
java {
    sourceCompatibility = JavaVersion.VERSION_25
    targetCompatibility = JavaVersion.VERSION_25
}

tasks.withType<JavaCompile> {
    options.release = 25
}

val shade: Configuration by configurations.creating

dependencies {
    minecraft("com.mojang:minecraft:${libs.versions.minecraft.fabric.get()}")
    implementation("net.fabricmc:fabric-loader:${libs.versions.fabric.loader.get()}")
    implementation("net.fabricmc.fabric-api:fabric-api:${libs.versions.fabric.api.get()}")

    shade(project(":core"))
    implementation(project(":core"))
}

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

tasks.build {
    dependsOn(tasks.shadowJar)
}
