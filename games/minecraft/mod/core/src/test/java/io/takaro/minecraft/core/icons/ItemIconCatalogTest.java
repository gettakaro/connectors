package io.takaro.minecraft.core.icons;

import io.takaro.minecraft.core.map.PngWriter;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.io.TempDir;

import javax.imageio.ImageIO;
import java.awt.image.BufferedImage;
import java.io.ByteArrayInputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.HexFormat;
import java.util.List;

import static org.junit.jupiter.api.Assertions.*;

class ItemIconCatalogTest {

    private static final byte[] PNG = PngWriter.write(new int[]{0xff112233}, 1, 1);

    @TempDir
    Path mod;

    private void file(String relative, byte[] bytes) throws Exception {
        Path file = mod.resolve(relative);
        Files.createDirectories(file.getParent());
        Files.write(file, bytes);
    }

    private void json(String relative, String text) throws Exception {
        file(relative, text.getBytes(StandardCharsets.UTF_8));
    }

    private static byte[] png(int fill) {
        return PngWriter.write(new int[]{fill}, 1, 1);
    }

    /** A valid PNG header and exactly {@code size} bytes; only animated textures get decoded. */
    private static byte[] pngOfSize(int size, int seed) {
        byte[] bytes = new byte[size];
        System.arraycopy(PNG, 0, bytes, 0, 33);
        bytes[size - 1] = (byte) seed;
        bytes[size - 2] = (byte) (seed >> 8);
        return bytes;
    }

    private static String sha256(byte[] bytes) throws Exception {
        return HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(bytes));
    }

    private ItemIconCatalog built(String... items) {
        ItemIconCatalog catalog = new ItemIconCatalog(List.of(mod), s -> {}, s -> {});
        catalog.build(List.of(items));
        assertTrue(catalog.ready().isDone());
        return catalog;
    }

    @Test
    void anItemDefinitionLeadsThroughItsModelToTheTexture() throws Exception {
        json("assets/mymod/items/ruby.json", "{\"model\":{\"type\":\"minecraft:model\",\"model\":\"mymod:item/ruby\"}}");
        json("assets/mymod/models/item/ruby.json", "{\"parent\":\"minecraft:item/generated\",\"textures\":{\"layer0\":\"mymod:item/gem\"}}");
        file("assets/mymod/textures/item/gem.png", PNG);

        assertEquals(sha256(PNG), built("mymod:ruby").hashFor("mymod:ruby"));
    }

    @Test
    void aConditionalDefinitionUsesItsRestingState() throws Exception {
        json("assets/mymod/items/bow.json", "{\"model\":{\"type\":\"minecraft:condition\",\"property\":\"minecraft:using_item\","
                + "\"on_true\":{\"type\":\"minecraft:model\",\"model\":\"mymod:item/bow_drawn\"},"
                + "\"on_false\":{\"type\":\"minecraft:model\",\"model\":\"mymod:item/bow\"}}}");
        json("assets/mymod/models/item/bow.json", "{\"textures\":{\"layer0\":\"mymod:item/bow\"}}");
        json("assets/mymod/models/item/bow_drawn.json", "{\"textures\":{\"layer0\":\"mymod:item/bow_drawn\"}}");
        file("assets/mymod/textures/item/bow.png", png(0xff000001));
        file("assets/mymod/textures/item/bow_drawn.png", png(0xff000002));

        assertEquals(sha256(png(0xff000001)), built("mymod:bow").hashFor("mymod:bow"));
    }

    @Test
    void aBlockItemFindsItsFaceThroughParentsAndReferences() throws Exception {
        json("assets/mymod/items/ore.json", "{\"model\":{\"type\":\"minecraft:model\",\"model\":\"mymod:block/ore\"}}");
        json("assets/mymod/models/block/ore.json", "{\"parent\":\"mymod:block/base\",\"textures\":{\"all\":\"#stone\"}}");
        json("assets/mymod/models/block/base.json", "{\"parent\":\"minecraft:block/cube_all\",\"textures\":{\"stone\":{\"sprite\":\"mymod:block/ore\"}}}");
        file("assets/mymod/textures/block/ore.png", PNG);

        assertEquals(sha256(PNG), built("mymod:ore").hashFor("mymod:ore"));
    }

    @Test
    void anItemWithoutModelsFallsBackToTheConventionalPath() throws Exception {
        file("assets/mymod/textures/item/plain.png", PNG);

        assertEquals(sha256(PNG), built("mymod:plain").hashFor("mymod:plain"));
    }

    @Test
    void aUvSheetIsNotAnIcon() throws Exception {
        json("assets/mymod/models/item/wrench.json", "{\"elements\":[],\"textures\":{\"0\":\"mymod:item/sheet\",\"particle\":\"mymod:item/sheet\"}}");
        file("assets/mymod/textures/item/sheet.png", PNG);

        assertNull(built("mymod:wrench").hashFor("mymod:wrench"));
    }

    @Test
    void vanillaItemsAndUnknownItemsHaveNoHash() throws Exception {
        file("assets/minecraft/textures/item/diamond.png", PNG);

        ItemIconCatalog catalog = built("minecraft:diamond", "mymod:missing");
        assertNull(catalog.hashFor("minecraft:diamond"));
        assertNull(catalog.hashFor("mymod:missing"));
    }

    @Test
    void aModelCannotClimbOutOfTheAssetsFolder() throws Exception {
        json("assets/mymod/models/item/evil.json", "{\"textures\":{\"layer0\":\"mymod:../../../secret\"}}");
        file("secret.png", PNG);

        assertNull(built("mymod:evil").hashFor("mymod:evil"));
    }

    @Test
    void aFileThatIsNotPngHasNoHash() throws Exception {
        file("assets/mymod/textures/item/fake.png", "not a png".getBytes(StandardCharsets.UTF_8));

        assertNull(built("mymod:fake").hashFor("mymod:fake"));
    }

    @Test
    void aSignatureWithoutAHeaderHasNoHash() throws Exception {
        file("assets/mymod/textures/item/stub.png", java.util.Arrays.copyOf(PNG, 8));

        assertNull(built("mymod:stub").hashFor("mymod:stub"));
    }

    @Test
    void anAnimatedImageClaimingAnEnormousSizeIsNotDecoded() throws Exception {
        byte[] bomb = PNG.clone();
        java.nio.ByteBuffer.wrap(bomb).putInt(16, 100_000).putInt(20, 100_000);
        file("assets/mymod/textures/item/bomb.png", bomb);
        json("assets/mymod/textures/item/bomb.png.mcmeta", "{\"animation\":{}}");

        assertNull(built("mymod:bomb").hashFor("mymod:bomb"));
    }

    @Test
    void aSourceFileOverTheReadLimitHasNoHash() throws Exception {
        file("assets/mymod/textures/item/giant.png", pngOfSize((8 << 20) + 1, 1));

        assertNull(built("mymod:giant").hashFor("mymod:giant"));
    }

    @Test
    void anAnimatedStripServesItsFirstFrame() throws Exception {
        int[] pixels = new int[2 * 4];
        java.util.Arrays.fill(pixels, 0, 4, 0xffaa0000);
        java.util.Arrays.fill(pixels, 4, 8, 0xff00aa00);
        file("assets/mymod/textures/item/magic.png", PngWriter.write(pixels, 2, 4));
        json("assets/mymod/textures/item/magic.png.mcmeta", "{\"animation\":{\"frametime\":2}}");

        ItemIconCatalog catalog = built("mymod:magic");
        String hash = catalog.hashFor("mymod:magic");
        byte[] served = catalog.serve(List.of(hash)).get(0).png();

        assertEquals(hash, sha256(served));
        BufferedImage image = ImageIO.read(new ByteArrayInputStream(served));
        assertEquals(2, image.getWidth());
        assertEquals(2, image.getHeight());
        assertEquals(0xffaa0000, image.getRGB(0, 0));
    }

    @Test
    void theHashIsTheSameOnEveryBuild() throws Exception {
        file("assets/mymod/textures/item/plain.png", PNG);
        json("assets/mymod/textures/item/plain.png.mcmeta", "{}");

        assertEquals(built("mymod:plain").hashFor("mymod:plain"), built("mymod:plain").hashFor("mymod:plain"));
    }

    @Test
    void sharedArtIsOneHash() throws Exception {
        json("assets/mymod/models/item/a.json", "{\"textures\":{\"layer0\":\"mymod:item/same\"}}");
        json("assets/mymod/models/item/b.json", "{\"textures\":{\"layer0\":\"mymod:item/same\"}}");
        file("assets/mymod/textures/item/same.png", PNG);

        ItemIconCatalog catalog = built("mymod:a", "mymod:b");
        assertEquals(catalog.hashFor("mymod:a"), catalog.hashFor("mymod:b"));
    }

    @Test
    void anEarlierRootWinsAConflict() throws Exception {
        Path other = mod.resolve("other");
        Files.createDirectories(other.resolve("assets/mymod/textures/item"));
        Files.write(other.resolve("assets/mymod/textures/item/plain.png"), png(0xff000001));
        file("assets/mymod/textures/item/plain.png", png(0xff000002));

        ItemIconCatalog catalog = new ItemIconCatalog(List.of(other, mod), s -> {}, s -> {});
        catalog.build(List.of("mymod:plain"));

        assertEquals(sha256(png(0xff000001)), catalog.hashFor("mymod:plain"));
    }

    @Test
    void aResponseStopsBeforeTheBoundAndKeepsRequestOrder() throws Exception {
        int size = 400 * 1024;
        List<String> items = new ArrayList<>();
        for (int i = 0; i < 4; i++) {
            file("assets/mymod/textures/item/big" + i + ".png", pngOfSize(size, i));
            items.add("mymod:big" + i);
        }
        ItemIconCatalog catalog = built(items.toArray(String[]::new));
        List<String> hashes = items.stream().map(catalog::hashFor).toList();

        List<ItemIconCatalog.Icon> first = catalog.serve(hashes);
        assertEquals(hashes.subList(0, 2), first.stream().map(ItemIconCatalog.Icon::hash).toList());
        assertTrue(first.stream().mapToInt(i -> i.png().length).sum() <= ItemIconCatalog.MAX_RESPONSE_BYTES);

        List<ItemIconCatalog.Icon> rest = catalog.serve(hashes.subList(2, 4));
        assertEquals(hashes.subList(2, 4), rest.stream().map(ItemIconCatalog.Icon::hash).toList());
    }

    @Test
    void anIconOverTheBoundIsNeverReported() throws Exception {
        file("assets/mymod/textures/item/huge.png", pngOfSize(ItemIconCatalog.MAX_RESPONSE_BYTES + 1, 1));
        file("assets/mymod/textures/item/exact.png", pngOfSize(ItemIconCatalog.MAX_RESPONSE_BYTES, 2));

        ItemIconCatalog catalog = built("mymod:huge", "mymod:exact");
        assertNull(catalog.hashFor("mymod:huge"));
        assertEquals(1, catalog.serve(List.of(catalog.hashFor("mymod:exact"))).size());
    }

    @Test
    void anUnknownHashIsSkippedAndTheWalkContinues() throws Exception {
        file("assets/mymod/textures/item/plain.png", PNG);
        ItemIconCatalog catalog = built("mymod:plain");
        String hash = catalog.hashFor("mymod:plain");

        List<ItemIconCatalog.Icon> answer = catalog.serve(List.of("0".repeat(64), hash));
        assertEquals(1, answer.size());
        assertEquals(hash, answer.get(0).hash());
    }

    @Test
    void aFileThatChangedAfterTheBuildIsSkippedNotServedUnderTheOldHash() throws Exception {
        file("assets/mymod/textures/item/plain.png", png(0xff000001));
        ItemIconCatalog catalog = built("mymod:plain");
        String hash = catalog.hashFor("mymod:plain");

        file("assets/mymod/textures/item/plain.png", png(0xff000002));
        assertEquals(List.of(), catalog.serve(List.of(hash)));
    }
}
