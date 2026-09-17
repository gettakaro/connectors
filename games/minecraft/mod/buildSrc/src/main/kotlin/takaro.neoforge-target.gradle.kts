import io.takaro.gradle.TargetCatalog
import java.security.MessageDigest
import org.gradle.api.attributes.Bundling
import org.gradle.api.attributes.Category
import org.gradle.api.attributes.LibraryElements
import org.gradle.api.attributes.Usage

// Everything a NeoForge target needs beyond takaro.target-base. Gradle's own types only:
// ModDevGradle and shadow are applied by the target's own build file.
plugins {
    id("takaro.target-base")
}

val target = TargetCatalog.load(project)

repositories {
    maven("https://maven.neoforged.net/releases")
}

// The NeoForge sources are shared by every NeoForge target; a target may overlay its own
// sources under targets/<id>/src/main/java when a game version needs different code.
sourceSets.named("main") {
    java.srcDir(layout.projectDirectory.dir("../../neoforge/src/main/java"))
    resources.srcDir(layout.projectDirectory.dir("../../neoforge/src/main/resources"))
}

sourceSets.named("test") {
    java.srcDir(layout.projectDirectory.dir("../../neoforge/src/test/java"))
}

val shade: Configuration by configurations.creating

dependencies {
    "implementation"(project(":core"))
    shade(project(":core"))
    "testImplementation"("org.junit.jupiter:junit-jupiter:6.1.0")
    "testRuntimeOnly"("org.junit.platform:junit-platform-launcher")
}

tasks.withType<Test>().configureEach {
    useJUnitPlatform()
}

// NeoForge publishes no plain jar and its POM is packaging=pom, so `verifyTargetInputs`
// cannot hash `net.neoforged:neoforge` from build.deps. The two jars the catalog pins as
// inputs are resolved through their published variants and hashed here instead.
val nfVersion = target.input("loader")["loaderVersion"] as String

val verifyNeoForgeInputs by tasks.registering {
    val expectedUniversal = target.input("universal")["sha256"] as String
    val expectedInstaller = target.input("loader")["sha256"] as String
    inputs.property("version", nfVersion)
    inputs.property("universal", expectedUniversal)
    inputs.property("installer", expectedInstaller)
    val outputFile = layout.buildDirectory.file("takaro/verified-neoforge-inputs.txt")
    outputs.file(outputFile)
    doLast {
        val universal = configurations.detachedConfiguration(
            dependencies.create("net.neoforged:neoforge:$nfVersion")
        ).apply {
            isTransitive = false
            attributes {
                attribute(Category.CATEGORY_ATTRIBUTE, objects.named(Category::class.java, Category.LIBRARY))
                attribute(Usage.USAGE_ATTRIBUTE, objects.named(Usage::class.java, Usage.JAVA_RUNTIME))
                attribute(
                    LibraryElements.LIBRARY_ELEMENTS_ATTRIBUTE,
                    objects.named(LibraryElements::class.java, LibraryElements.JAR)
                )
                attribute(Bundling.BUNDLING_ATTRIBUTE, objects.named(Bundling::class.java, Bundling.EXTERNAL))
            }
        }
        val installer = configurations.detachedConfiguration(
            (dependencies.create("net.neoforged:neoforge:$nfVersion") as ModuleDependency).apply {
                capabilities { requireCapability("net.neoforged:neoforge-installer:$nfVersion") }
            }
        ).apply { isTransitive = false }

        val problems = mutableListOf<String>()
        val lines = mutableListOf<String>()
        for ((name, pair) in listOf(
            "universal" to (universal to expectedUniversal),
            "installer" to (installer to expectedInstaller)
        )) {
            val (configuration, expected) = pair
            val files = configuration.resolve().sortedBy { it.name }
            if (files.isEmpty()) {
                problems += "$name: nothing resolved for net.neoforged:neoforge:$nfVersion"
                continue
            }
            for (file in files) {
                val digest = MessageDigest.getInstance("SHA-256")
                    .digest(file.readBytes())
                    .joinToString("") { String.format("%02x", it) }
                if (digest != expected) {
                    problems += "$name (${file.name}): expected $expected actual $digest"
                } else {
                    lines += "$name ${file.name} $digest"
                }
            }
        }
        if (problems.isNotEmpty()) {
            throw GradleException(
                "NeoForge inputs do not match the catalog:\n  " + problems.joinToString("\n  ")
            )
        }
        val file = outputFile.get().asFile
        file.parentFile.mkdirs()
        file.writeText(lines.joinToString("\n") + "\n")
    }
}

tasks.named("compileJava") {
    dependsOn(verifyNeoForgeInputs)
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
            "the NeoForge target produced $produced but the catalog names its server-mod $expected"
        }
    }
}

tasks.named("build") {
    dependsOn("shadowJar")
}
