package io.takaro.zomboid.agent;

import io.takaro.zomboid.core.model.GameItem;
import io.takaro.zomboid.core.model.InventoryItem;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.function.BiFunction;

/**
 * Pure (game-class-free) transforms for the item catalogue and a player's
 * inventory. Keeping them out of {@link Pz} lets them be unit-tested without a
 * running game world: {@link Pz} reads the raw game objects into the small rows
 * below, and these methods shape and de-duplicate them.
 */
public final class Catalog {

    private Catalog() {
    }

    /** One raw row read from {@code ScriptManager.getAllItems()}. */
    public record ItemRow(String fullName, String displayName, String displayCategory) {
    }

    /** One raw row read from an {@code ItemContainer.getItems()} entry. */
    public record InvRow(String fullType, String displayName, int count, int condition, int conditionMax) {
    }

    /**
     * Build the Takaro {@code listItems} payload: {@code code}=fullName,
     * {@code name}=displayName (fallback to code), {@code description}=category.
     * Rows with a null/blank code are skipped, and duplicate codes are collapsed
     * (first occurrence wins) so the catalogue has no dupes.
     */
    public static List<GameItem> buildItems(List<ItemRow> rows) {
        Map<String, GameItem> byCode = new LinkedHashMap<>();
        if (rows == null) {
            return new ArrayList<>();
        }
        for (ItemRow row : rows) {
            if (row == null) {
                continue;
            }
            String code = row.fullName();
            if (code == null || code.isEmpty()) {
                continue;
            }
            if (byCode.containsKey(code)) {
                continue;
            }
            String name = row.displayName();
            if (name == null || name.isEmpty()) {
                name = code;
            }
            String description = row.displayCategory() == null ? "" : row.displayCategory();
            byCode.put(code, new GameItem(code, name, description));
        }
        return new ArrayList<>(byCode.values());
    }

    /**
     * Group a player's raw inventory items by {@code fullType}: the amount is the
     * summed {@code getCount()} of all stacks of that type, and the quality is the
     * first stack's {@code condition/conditionMax} (e.g. {@code "8/10"}; empty when
     * the item has no condition track). First occurrence sets name and quality.
     */
    public static List<InventoryItem> groupInventory(List<InvRow> rows) {
        Map<String, int[]> amounts = new LinkedHashMap<>();    // code -> {amount}
        Map<String, String> names = new LinkedHashMap<>();
        Map<String, String> qualities = new LinkedHashMap<>();
        if (rows == null) {
            return new ArrayList<>();
        }
        for (InvRow row : rows) {
            if (row == null) {
                continue;
            }
            String code = row.fullType();
            if (code == null || code.isEmpty()) {
                continue;
            }
            int add = Math.max(1, row.count());
            int[] acc = amounts.get(code);
            if (acc == null) {
                amounts.put(code, new int[] {add});
                String name = row.displayName();
                names.put(code, name == null || name.isEmpty() ? code : name);
                qualities.put(code, formatQuality(row.condition(), row.conditionMax()));
            } else {
                acc[0] += add;
            }
        }
        List<InventoryItem> out = new ArrayList<>();
        for (Map.Entry<String, int[]> e : amounts.entrySet()) {
            String code = e.getKey();
            out.add(new InventoryItem(code, names.get(code), e.getValue()[0], qualities.get(code)));
        }
        return out;
    }

    /**
     * A vehicle's player-facing name, resolved the way the game's own UI does it
     * ({@code ISVehicleMechanics.lua}): {@code IGUI_VehicleName<carModelName or script name>},
     * and for a {@code *Burnt*} script the unburnt model's name wrapped in
     * {@code IGUI_VehicleNameBurntCar}. {@code textOrNull} is {@code Translator.getTextOrNull}
     * (key, optional argument). Falls back to the script name when no translation exists.
     */
    public static String vehicleDisplayName(String scriptName, String carModelName,
                                            BiFunction<String, String, String> textOrNull) {
        String key = carModelName != null && !carModelName.isEmpty() ? carModelName : scriptName;
        String name = key == null ? null : textOrNull.apply("IGUI_VehicleName" + key, null);
        if (scriptName != null && scriptName.contains("Burnt")) {
            String unburnt = textOrNull.apply("IGUI_VehicleName" + scriptName.replace("Burnt", ""), null);
            if (unburnt != null) {
                name = unburnt;
            }
            if (name != null) {
                String burnt = textOrNull.apply("IGUI_VehicleNameBurntCar", name);
                if (burnt != null) {
                    name = burnt;
                }
            }
        }
        if ((name == null || name.isBlank()) && key != null) {
            // Variants PZ ships without a translation (ModernCarSmashedLeft, SportsCar_ez):
            // name them after their base model, as the translated CarLightsSmashed* ones are.
            String base = key.replaceFirst("_[^_]+$", "");
            boolean smashed = base.matches(".*Smashed(Front|Rear|Left|Right)$");
            base = base.replaceFirst("Smashed(Front|Rear|Left|Right)$", "");
            String baseName = base.equals(key) ? null : textOrNull.apply("IGUI_VehicleName" + base, null);
            if (baseName != null) {
                name = smashed ? "Wrecked " + baseName : baseName;
            }
        }
        return name == null || name.isBlank() ? scriptName : name;
    }

    /** Quality as {@code condition/conditionMax}; blank when the item has no condition track. */
    public static String formatQuality(int condition, int conditionMax) {
        if (conditionMax <= 0) {
            return "";
        }
        return condition + "/" + conditionMax;
    }
}
