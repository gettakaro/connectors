package io.takaro.zomboid.agent;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import io.takaro.zomboid.core.model.GameItem;
import io.takaro.zomboid.core.model.ItemIcon;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

class ItemIconCatalogTest {

    private static final int ONE_MIB = ItemIconCatalog.MAX_RESPONSE_BYTES;

    @TempDir
    Path dir;

    private final ItemIconCatalog catalog = new ItemIconCatalog(line -> { });

    private String png(String name, int size) throws IOException {
        byte[] bytes = new byte[size];
        byte[] signature = {(byte) 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
        System.arraycopy(signature, 0, bytes, 0, signature.length);
        bytes[size - 1] = (byte) name.hashCode();
        Path file = dir.resolve(name);
        Files.write(file, bytes);
        return file.toString();
    }

    @Test
    void reportsAHashOfTheExactBytesItServes() throws IOException {
        catalog.build(Map.of("Sword", png("a.png", 100)));
        String hash = catalog.hashFor("Sword");
        assertTrue(hash.matches("[0-9a-f]{64}"), hash);
        List<ItemIcon> answer = catalog.serve(List.of(hash));
        assertEquals(1, answer.size());
        assertArrayEquals(Files.readAllBytes(dir.resolve("a.png")), answer.get(0).png());
    }

    @Test
    void hashIsStableAcrossABuildFromTheSameFiles() throws IOException {
        Map<String, String> files = Map.of("Sword", png("a.png", 100));
        catalog.build(files);
        ItemIconCatalog afterRestart = new ItemIconCatalog(line -> { });
        afterRestart.build(files);
        assertEquals(catalog.hashFor("Sword"), afterRestart.hashFor("Sword"));
    }

    @Test
    void itemsSharingArtShareAHash() throws IOException {
        String file = png("a.png", 100);
        catalog.build(Map.of("One", file, "Two", file));
        assertEquals(catalog.hashFor("One"), catalog.hashFor("Two"));
    }

    @Test
    void anItemWithoutArtReportsNoHash() {
        catalog.build(Map.of());
        assertNull(catalog.hashFor("Missing"));
        assertNull(catalog.hashFor(null));
    }

    @Test
    void aFileThatIsNotAPngGetsNoHash() throws IOException {
        Path file = dir.resolve("fake.png");
        Files.writeString(file, "not an image");
        catalog.build(Map.of("Fake", file.toString()));
        assertNull(catalog.hashFor("Fake"));
    }

    @Test
    void anIconOverTheBoundGetsNoHash() throws IOException {
        catalog.build(Map.of("Huge", png("huge.png", ONE_MIB + 1)));
        assertNull(catalog.hashFor("Huge"));
    }

    @Test
    void anIconOfExactlyTheBoundIsServed() throws IOException {
        catalog.build(Map.of("Big", png("big.png", ONE_MIB)));
        assertEquals(1, catalog.serve(List.of(catalog.hashFor("Big"))).size());
    }

    @Test
    void aResponseStopsBeforeBreakingTheBoundAndTakaroAsksForTheRest() throws IOException {
        Map<String, String> files = new HashMap<>();
        for (int i = 0; i < 3; i++) {
            files.put("Icon" + i, png("icon" + i + ".png", (ONE_MIB / 2) - i));
        }
        files.put("Small", png("small.png", 100));
        catalog.build(files);
        List<String> hashes = List.of(catalog.hashFor("Icon0"), catalog.hashFor("Icon1"),
                catalog.hashFor("Icon2"), catalog.hashFor("Small"));

        List<ItemIcon> first = catalog.serve(hashes);
        assertEquals(List.of(hashes.get(0), hashes.get(1)), first.stream().map(ItemIcon::hash).toList());
        assertTrue(first.stream().mapToLong(i -> i.png().length).sum() <= ONE_MIB);

        List<ItemIcon> second = catalog.serve(hashes.subList(2, 4));
        assertEquals(hashes.subList(2, 4), second.stream().map(ItemIcon::hash).toList());
    }

    @Test
    void anUnknownHashIsSkippedAndTheRestStillAnswered() throws IOException {
        catalog.build(Map.of("Sword", png("a.png", 100)));
        String known = catalog.hashFor("Sword");
        List<ItemIcon> answer = catalog.serve(List.of("0".repeat(64), known));
        assertEquals(List.of(known), answer.stream().map(ItemIcon::hash).toList());
    }

    @Test
    void aFileChangedAfterHashingIsSkippedNotServedUnderTheOldHash() throws IOException {
        String file = png("a.png", 100);
        catalog.build(Map.of("Sword", file));
        String hash = catalog.hashFor("Sword");
        png("a.png", 200);
        assertEquals(List.of(), catalog.serve(List.of(hash)));
    }

    @Test
    void aFileDeletedAfterHashingIsSkipped() throws IOException {
        String file = png("a.png", 100);
        catalog.build(Map.of("Sword", file));
        String hash = catalog.hashFor("Sword");
        Files.delete(Path.of(file));
        assertEquals(List.of(), catalog.serve(List.of(hash)));
    }

    @Test
    void catalogPutsTheHashOnTheItemAndOmitsItWithoutArt() {
        List<Catalog.ItemRow> rows = new ArrayList<>();
        rows.add(new Catalog.ItemRow("Mod.Sword", "Sword", "Weapon", "ModSword"));
        rows.add(new Catalog.ItemRow("Base.Axe", "Axe", "Weapon", "Axe"));
        List<GameItem> items = Catalog.buildItems(rows, icon -> icon.equals("ModSword") ? "ab".repeat(32) : null);
        assertNotNull(items.get(0).iconHash());
        assertNull(items.get(1).iconHash());
    }
}
