#pragma once

// StarRupture does not always mirror a newly possessed character into
// APlayerState::PawnPrivate immediately. Prefer that direct link, then fall
// back to the authoritative player controller used by the action boundary.
template <typename PlayerState>
auto ResolveTakaroPlayerPawn(PlayerState* playerState) -> decltype(playerState->GetPawn())
{
    if (!playerState) return nullptr;
    if (auto* pawn = playerState->GetPawn()) return pawn;
    auto* controller = playerState->GetPlayerController();
    return controller ? controller->K2_GetPawn() : nullptr;
}
