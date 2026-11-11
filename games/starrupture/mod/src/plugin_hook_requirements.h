#pragma once

inline bool EssentialTakaroHookGroupsAvailable(bool hasEngine, bool hasWorld, bool hasPlayers)
{
    return hasEngine && hasWorld && hasPlayers;
}
