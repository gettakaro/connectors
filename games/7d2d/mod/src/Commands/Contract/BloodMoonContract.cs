namespace Takaro.Commands.Contract
{
    /// <summary>The PrismaCore output contract of <c>isbloodmoon</c>.</summary>
    public static class BloodMoonContract
    {
        public static readonly string[] Names = { "isbloodmoon" };

        public static string Line(bool isBloodMoon) => isBloodMoon ? "true" : "false";
    }
}
