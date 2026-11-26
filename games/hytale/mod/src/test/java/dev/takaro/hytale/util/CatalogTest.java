package dev.takaro.hytale.util;

import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.*;

class CatalogTest {

    @Test
    void developerScaffoldingIsRecognised() {
        assertTrue(Catalog.isDeveloperAsset("Debug_Donut_Selector"));
        assertTrue(Catalog.isDeveloperAsset("Debug_Forking_2"));
        assertTrue(Catalog.isDeveloperAsset("Test_Block"));
        assertTrue(Catalog.isDeveloperAsset("Dev_Marker"));
        assertTrue(Catalog.isDeveloperAsset("debug_lowercase"));
        // 0.7.7: test and template assets with the marker in the middle or at the end.
        assertTrue(Catalog.isDeveloperAsset("Glass_Test_Red"));
        assertTrue(Catalog.isDeveloperAsset("Weapon_Shortbow_Test_Zoom"));
        assertTrue(Catalog.isDeveloperAsset("Furniture_Crude_Windows_1X2_Test"));
        assertTrue(Catalog.isDeveloperAsset("Template_Weapon_Sword"));
        assertTrue(Catalog.isDeveloperAsset("Potion_Template"));
    }

    @Test
    void realContentIsKept() {
        assertFalse(Catalog.isDeveloperAsset("Armor_Adamantite_Chest"));
        assertFalse(Catalog.isDeveloperAsset("Wood_Oak_Trunk"));
        // Only a whole segment counts - a real item that merely contains the word stays in.
        assertFalse(Catalog.isDeveloperAsset("Tool_Debugger"));
        assertFalse(Catalog.isDeveloperAsset("Device_Panel"));
        assertFalse(Catalog.isDeveloperAsset(null));
        assertFalse(Catalog.isDeveloperAsset(""));
        assertFalse(Catalog.isDeveloperAsset("Fluid_Water"));
        assertFalse(Catalog.isDeveloperAsset("Testudo_Shell"));
    }

    @Test
    void namesVariantsAfterTheirTranslatedBase() {
        assertEquals(java.util.List.of("Skeleton_Sand_Mage", "Skeleton_Sand", "Skeleton"),
            Catalog.roleBaseCandidates("Skeleton_Sand_Mage_Patrol"));
        assertEquals(java.util.List.of(), Catalog.roleBaseCandidates("Pig"));
        assertEquals("Sandswept Skeleton Mage (Patrol)",
            Catalog.variantName("Sandswept Skeleton Mage", "Skeleton_Sand_Mage", "Skeleton_Sand_Mage_Patrol"));
        assertEquals("Revenant Guardian (Phase 2)",
            Catalog.variantName("Revenant Guardian", "Skeleton_Elite", "Skeleton_Elite_Phase_2"));
    }

    @Test
    void dropsScaffoldingRolesThatAreNotEntities() {
        assertTrue(Catalog.isPlaceholderRole("Static"));
        assertTrue(Catalog.isPlaceholderRole("Static2"));
        assertTrue(Catalog.isPlaceholderRole("Static4"));
        assertTrue(Catalog.isPlaceholderRole("Template"));
        assertTrue(Catalog.isPlaceholderRole("BlankTemplate"));
        assertTrue(Catalog.isPlaceholderRole("Empty_Role"));
        assertTrue(Catalog.isPlaceholderRole(null));
        assertFalse(Catalog.isPlaceholderRole("Bat_Ice"));
        assertFalse(Catalog.isPlaceholderRole("StaticTrork"));
    }

    @Test
    void buildsTheRoleTranslationKeyHytaleUses() {
        assertEquals(
            java.util.List.of("server.npcRoles.Bat_Ice.name", "npcRoles.Bat_Ice.name"),
            Catalog.roleNameKeys("Bat_Ice"));
    }

    @Test
    void humanisesOnlyAsALastResort() {
        assertEquals("Rex Cave", Catalog.humanise("Rex_Cave"));
        assertEquals("Trork Grunt", Catalog.humanise("trork_grunt"));
        assertEquals("Bat", Catalog.humanise("Bat"));
        assertEquals("", Catalog.humanise(null));
    }
}
