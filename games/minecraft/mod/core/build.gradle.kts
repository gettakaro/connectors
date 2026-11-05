plugins {
    `java-library`
}

dependencies {
    implementation(libs.java.websocket)
    implementation(libs.gson)
    compileOnly(libs.log4j.api)

    testImplementation("org.junit.jupiter:junit-jupiter:6.1.0")
    testRuntimeOnly("org.junit.platform:junit-platform-launcher")
}

tasks.test {
    useJUnitPlatform()
}

// The connector version and the Game Profile icon, read back by GameProfile from the classpath.
// Both come from files release-please and the repository own, never from a literal here.
val generatedResources = layout.buildDirectory.dir("generated/takaro-core")

val generateConnectorResources by tasks.registering {
    val outputDir = generatedResources
    val projectVersion = project.version.toString()
    val icon = rootProject.file("../assets/icon-256.png")
    inputs.property("version", projectVersion)
    inputs.file(icon)
    outputs.dir(outputDir)
    doLast {
        val dir = outputDir.get().dir("io/takaro/minecraft/core").asFile
        dir.mkdirs()
        dir.resolve("connector-version.txt").writeText(projectVersion + "\n")
        icon.copyTo(dir.resolve("icon-256.png"), overwrite = true)
    }
}

sourceSets.named("main") {
    resources.srcDir(generateConnectorResources.map { generatedResources })
}
