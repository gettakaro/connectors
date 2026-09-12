plugins {
    alias(libs.plugins.shadow)
}

dependencies {
    implementation(libs.byte.buddy)
    // Project Zomboid server classes — provided by the game JVM at runtime.
    // Staged by scripts/setup-environment.sh.
    compileOnly(files("../_deps/projectzomboid.jar"))
}

tasks.shadowJar {
    archiveBaseName.set("TakaroConnector")
    archiveClassifier.set("")
    relocate("net.bytebuddy", "io.takaro.zomboid.libs.bytebuddy")
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
