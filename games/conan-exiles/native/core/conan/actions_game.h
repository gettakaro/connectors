// The real MutationGame: every call is one queued game-thread job built from reflected
// UFunctions (spikes S1/S2 on build 25639945):
//   giveItem   ConanCharacter.GetBackpackInventory + ItemInventory.AddItemTemplate, read back with
//              GetNumberOfItemsByTemplate
//   teleport   ConanPlayerController.TeleportPlayerServer(RunCheatCheck=false), Actor.K2_GetActorLocation
//   kick       KismetTextLibrary.Conv_StringToText + PlayerController.ClientReturnToMainMenuWithTextReason
//   console    KismetSystemLibrary.ExecuteConsoleCommand on the first player's controller with
//              m_IsAdmin set and restored inside the same job (never reaches a net tick), or the
//              engine console when nobody is online; GetFrameCount tags the call's log lines
//   exit       KismetSystemLibrary.ExecuteConsoleCommand("exit") -> UGameEngine::HandleExitCommand
//   login hook GameModeBase.K2_PostLogin (and its Blueprint overrides) through conan/hook_dispatch.h
#pragma once

#include "conan/actions.h"

namespace conan {

std::shared_ptr<MutationGame> MakeUeMutationGame();

}  // namespace conan
