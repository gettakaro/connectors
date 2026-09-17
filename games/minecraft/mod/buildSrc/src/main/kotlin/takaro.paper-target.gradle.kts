import io.takaro.gradle.TargetCatalog

// Everything a Paper target needs beyond takaro.target-base. Gradle's own types only:
// shadow is applied by the target's own build file, not from buildSrc's classpath.
plugins {
    id("takaro.target-base")
}

val target = TargetCatalog.load(project)

repositories {
    maven("https://repo.papermc.io/repository/maven-public/")
}

// The Paper sources are shared by every Paper target; a target may overlay its own
// sources under targets/<id>/src/main/java when a game version needs different code.
sourceSets.named("main") {
    java.srcDir(layout.projectDirectory.dir("../../paper/src/main/java"))
    resources.srcDir(layout.projectDirectory.dir("../../paper/src/main/resources"))
}

sourceSets.named("test") {
    java.srcDir(layout.projectDirectory.dir("../../paper/src/test/java"))
}

val shade: Configuration by configurations.creating

dependencies {
    // The exact unique snapshot the record names; verifyTargetInputs (target-base) additionally
    // resolves the SNAPSHOT coordinate and fails the build if upstream re-published it.
    "compileOnly"(target.deps.getValue("paper-api")["resolvedCoordinate"] as String)
    "implementation"(project(":core"))
    shade(project(":core"))
    "testImplementation"("org.junit.jupiter:junit-jupiter:6.1.0")
    "testRuntimeOnly"("org.junit.platform:junit-platform-launcher")
}

tasks.withType<Test>().configureEach {
    useJUnitPlatform()
}

// A component's file name is part of its identity: `takaro-maint` looks the artifact up by
// this exact name instead of globbing build/libs, so a rename fails here first.
tasks.named<AbstractArchiveTask>("shadowJar") {
    doLast {
        @Suppress("UNCHECKED_CAST")
        val components = target.data["components"] as List<Map<String, Any?>>
        val pattern = components.first { it["role"] == "server-mod" }["artifact"] as String
        val expected = pattern.replace("{version}", project.version.toString())
        val produced = archiveFile.get().asFile.name
        check(produced == expected) {
            "the Paper target produced $produced but the catalog names its server-mod $expected"
        }
    }
}

tasks.named("build") {
    dependsOn("shadowJar")
}
