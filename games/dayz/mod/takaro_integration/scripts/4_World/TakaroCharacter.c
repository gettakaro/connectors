// A DayZ character lives until it dies; the account then starts a new one.
// The hive gives each character entity its own persistent id, which survives
// logout and server restarts and changes with every new life, so that id is
// the Takaro Character id. It is never derived from a name.
class TakaroCharacter
{
    // "" when the engine has not assigned an id, so callers report no character.
    static string Id(PlayerBase pb)
    {
        if (!pb) return "";
        int b1, b2, b3, b4;
        pb.GetPersistentID(b1, b2, b3, b4);
        if (b1 == 0 && b2 == 0 && b3 == 0 && b4 == 0) return "";
        return b1.ToString() + "_" + b2.ToString() + "_" + b3.ToString() + "_" + b4.ToString();
    }

    // The survivor's name in the game's own class: SurvivorF_Linda -> Linda.
    static string Name(PlayerBase pb)
    {
        if (!pb) return "";
        string type = pb.GetType();
        int sep = type.IndexOf("_");
        if (sep < 0) return type;
        return type.Substring(sep + 1, type.Length() - sep - 1);
    }

    // The `character` member of a player object, with its leading comma, or ""
    // when there is no id to report.
    static string JsonMember(PlayerBase pb)
    {
        string id = Id(pb);
        if (id == "") return "";
        string q = "\"";
        string s = "," + q + "character" + q + ":{" + q + "id" + q + ":" + q + id + q;
        s += "," + q + "name" + q + ":" + q + TakaroEventFactory.Safe(Name(pb)) + q + "}";
        return s;
    }
}
