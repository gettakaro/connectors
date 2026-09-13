package io.takaro.zomboid.agent;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import io.takaro.zomboid.core.model.GameItem;
import io.takaro.zomboid.core.model.InventoryItem;

import java.util.ArrayList;
import java.util.List;

import org.junit.jupiter.api.Test;

class CatalogTest {

    @Test
    void buildItemsMapsFieldsAndFallsBackToCodeForName() {
        List<Catalog.ItemRow> rows = List.of(
                new Catalog.ItemRow("Base.Axe", "Axe", "Weapon"),
                new Catalog.ItemRow("Base.Nails", "", "Material"));
        List<GameItem> items = Catalog.buildItems(rows);

        assertEquals(2, items.size());
        assertEquals("Base.Axe", items.get(0).code());
        assertEquals("Axe", items.get(0).name());
        assertEquals("Weapon", items.get(0).description());
        // blank display name falls back to the code
        assertEquals("Base.Nails", items.get(1).name());
    }

    @Test
    void buildItemsDeduplicatesByCodeAndSkipsBlankCodes() {
        List<Catalog.ItemRow> rows = new ArrayList<>();
        rows.add(new Catalog.ItemRow("Base.Axe", "Axe", "Weapon"));
        rows.add(new Catalog.ItemRow("Base.Axe", "Axe (dupe)", "Weapon")); // dupe -> dropped
        rows.add(new Catalog.ItemRow("", "No code", "X"));                  // blank -> dropped
        rows.add(null);                                                     // null -> dropped
        rows.add(new Catalog.ItemRow("Base.Hammer", "Hammer", "Tool"));

        List<GameItem> items = Catalog.buildItems(rows);
        assertEquals(2, items.size());
        assertEquals("Base.Axe", items.get(0).code());
        assertEquals("Axe", items.get(0).name()); // first occurrence wins
        assertEquals("Base.Hammer", items.get(1).code());
    }

    @Test
    void buildItemsHandlesNullList() {
        assertTrue(Catalog.buildItems(null).isEmpty());
    }

    @Test
    void groupInventorySumsCountsByFullType() {
        List<Catalog.InvRow> rows = List.of(
                new Catalog.InvRow("Base.Nails", "Nails", 5, 0, 0),
                new Catalog.InvRow("Base.Nails", "Nails", 3, 0, 0),
                new Catalog.InvRow("Base.Axe", "Axe", 1, 8, 10));
        List<InventoryItem> grouped = Catalog.groupInventory(rows);

        assertEquals(2, grouped.size());
        InventoryItem nails = grouped.get(0);
        assertEquals("Base.Nails", nails.code());
        assertEquals(8, nails.amount()); // 5 + 3
        assertEquals("", nails.quality()); // no condition track

        InventoryItem axe = grouped.get(1);
        assertEquals("Base.Axe", axe.code());
        assertEquals(1, axe.amount());
        assertEquals("8/10", axe.quality());
    }

    @Test
    void groupInventoryTreatsZeroCountAsOne() {
        List<Catalog.InvRow> rows = List.of(
                new Catalog.InvRow("Base.Axe", "Axe", 0, 10, 10));
        List<InventoryItem> grouped = Catalog.groupInventory(rows);
        assertEquals(1, grouped.get(0).amount());
    }

    @Test
    void formatQuality() {
        assertEquals("8/10", Catalog.formatQuality(8, 10));
        assertEquals("", Catalog.formatQuality(5, 0));
        assertEquals("", Catalog.formatQuality(0, -1));
    }
}
