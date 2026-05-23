class CfgPatches
{
    class TakaroIntegration
    {
        units[] = {};
        weapons[] = {};
        requiredVersion = 0.1;
        requiredAddons[] = {"DZ_Data", "DZ_Scripts"};
    };
};

class CfgMods
{
    class TakaroIntegration
    {
        type = "mod";
        dir = "TakaroIntegration";
        name = "TakaroIntegration";
        credits = "";
        author = "mad-001";
        authorID = "0";
        version = "0.1.14";
        extra = 0;
        dependencies[] = {"Game", "World", "Mission"};

        class defs
        {
            class engineScriptModule
            {
                value = "";
                files[] = {"TakaroIntegration/scripts/1_Core"};
            };
            class gameScriptModule
            {
                value = "";
                files[] = {"TakaroIntegration/scripts/3_Game"};
            };
            class worldScriptModule
            {
                value = "";
                files[] = {"TakaroIntegration/scripts/4_World"};
            };
            class missionScriptModule
            {
                value = "";
                files[] = {"TakaroIntegration/scripts/5_Mission"};
            };
        };
    };
};
