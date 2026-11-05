using System.Globalization;
using System.Text;
using Takaro.Valheim.Companion.Protocol;

namespace Takaro.Valheim.Companion;

/// <summary>
/// Turns the local character's skill list into a report, and says when one is due: on a change,
/// or after the refresh interval so a lost report heals itself.
/// </summary>
public sealed class CompanionSkillsReader
{
    private string? lastSentFingerprint;
    private TimeSpan? lastSentAt;

    public bool TryGetChangedOrRefresh(
        IReadOnlyList<CompanionSkill>? skills,
        TimeSpan monotonicNow,
        TimeSpan refreshInterval,
        out CompanionSkillsReport? report)
    {
        report = null;
        if (skills is null)
        {
            return false;
        }

        var normalized = skills
            .Where(IsReportable)
            .GroupBy(skill => skill.Code, StringComparer.Ordinal)
            .Select(group => group.First())
            .OrderBy(skill => skill.Code, StringComparer.Ordinal)
            .Take(CompanionProtocol.MaximumSkills)
            .Select(skill => skill with
            {
                Level = Math.Min(skill.Level, CompanionProtocol.MaximumSkillLevel),
                Accumulator = Math.Min(skill.Accumulator, CompanionProtocol.MaximumSkillAccumulator),
                Progress = Math.Min(skill.Progress, 1f)
            })
            .ToArray();
        var refreshDue = lastSentAt is TimeSpan sentAt && monotonicNow - sentAt >= refreshInterval;
        if (!refreshDue && string.Equals(Fingerprint(normalized), lastSentFingerprint, StringComparison.Ordinal))
        {
            return false;
        }

        report = new CompanionSkillsReport(normalized);
        return true;
    }

    public void MarkSent(CompanionSkillsReport report, TimeSpan monotonicNow)
    {
        if (report is null)
        {
            throw new ArgumentNullException(nameof(report));
        }

        lastSentFingerprint = Fingerprint(report.Skills);
        lastSentAt = monotonicNow;
    }

    public void Reset()
    {
        lastSentFingerprint = null;
        lastSentAt = null;
    }

#if TAKARO_VALHEIM_COMPANION
    internal bool TryReadChangedOrRefresh(
        Player? player,
        TimeSpan monotonicNow,
        TimeSpan refreshInterval,
        out CompanionSkillsReport? report)
    {
        report = null;
        var skills = player == null ? null : player.GetSkills();
        if (skills == null)
        {
            return false;
        }

        var entries = skills.GetSkillList()
            .Where(skill => skill.m_info != null)
            .Select(skill => new CompanionSkill(
                skill.m_info.m_skill.ToString(),
                skill.m_level,
                skill.m_accumulator,
                skill.GetLevelPercentage()))
            .ToArray();
        return TryGetChangedOrRefresh(entries, monotonicNow, refreshInterval, out report);
    }
#endif

    private static bool IsReportable(CompanionSkill skill) =>
        skill is not null
        && !string.IsNullOrWhiteSpace(skill.Code)
        && skill.Code.Length <= CompanionProtocol.MaximumCodeCharacters
        && IsNonNegative(skill.Level)
        && IsNonNegative(skill.Accumulator)
        && IsNonNegative(skill.Progress);

    private static bool IsNonNegative(float value) =>
        !float.IsNaN(value) && !float.IsInfinity(value) && value >= 0;

    private static string Fingerprint(IReadOnlyList<CompanionSkill> skills)
    {
        var canonical = new StringBuilder();
        foreach (var skill in skills)
        {
            canonical.Append(skill.Code).Append('|')
                .Append(skill.Level.ToString("R", CultureInfo.InvariantCulture)).Append('|')
                .Append(skill.Accumulator.ToString("R", CultureInfo.InvariantCulture)).Append('|')
                .Append(skill.Progress.ToString("R", CultureInfo.InvariantCulture)).Append(';');
        }

        return canonical.ToString();
    }
}
