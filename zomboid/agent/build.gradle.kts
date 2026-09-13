plugins {
    alias(libs.plugins.shadow)
}

dependencies {
    implementation(project(":core"))
    implementation(libs.byte.buddy)
    // core's WebSocket + JSON libs are used directly by the agent (BanStore,
    // config) and must be on the agent compile + runtime classpath.
    implementation(libs.java.websocket)
    implementation(libs.gson)
    // Project Zomboid server classes — provided by the game JVM at runtime.
    // Staged by scripts/setup-environment.sh.
    compileOnly(files("../_deps/projectzomboid.jar"))

    testImplementation(libs.junit.jupiter)
    testRuntimeOnly(libs.junit.platform.launcher)
    testImplementation(libs.gson)
}

tasks.test {
    useJUnitPlatform()
}

// The agent links against projectzomboid.jar (class-file v69 / Java 25), which
// a --release 21 javac refuses to read. Compile the agent to 25; it only ever
// runs inside the PZ server's Java 25 JVM anyway.
tasks.withType<JavaCompile> {
    options.release = 25
}

tasks.shadowJar {
    archiveBaseName.set("TakaroConnector")
    archiveClassifier.set("")
    // Relocate every bundled third-party package under io.takaro.zomboid.libs
    // so nothing collides with classes the game JVM already loads.
    relocate("net.bytebuddy", "io.takaro.zomboid.libs.bytebuddy")
    relocate("org.objectweb.asm", "io.takaro.zomboid.libs.asm")
    relocate("org.java_websocket", "io.takaro.zomboid.libs.org.java_websocket")
    relocate("com.google.gson", "io.takaro.zomboid.libs.com.google.gson")
    manifest {
        attributes(
            "Premain-Class" to "io.takaro.zomboid.agent.TakaroAgent",
            "Agent-Class" to "io.takaro.zomboid.agent.TakaroAgent",
            "Can-Retransform-Classes" to "true",
            "Can-Redefine-Classes" to "true",
            "Implementation-Title" to "Takaro Project Zomboid Connector",
            "Implementation-Version" to project.version.toString(),
            "ByteBuddy-Version" to libs.versions.byte.buddy.get(),
        )
    }
}

tasks.build {
    dependsOn(tasks.shadowJar)
}
