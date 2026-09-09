// GameOffsets.h - every build-specific number the payload uses, per A2 build.
//
// Selected by HALCYON_GAME_BUILD, which the project sets from $(GameBuild) (see the
// .vcxproj; default 22284). Each block is one game binary. Nothing in dllmain.cpp may
// spell out an RVA or a struct offset directly; it names an HX:: constant instead, so
// porting to a new build is: dump the SDK, run tools\port_offsets.py, verify in IDA,
// add a block here.
//
// Two kinds of number live here:
//   *_RVA   code/data addresses relative to the module base (from IDA / port_offsets.py)
//   others  struct member offsets (from the Dumper-7 SDK for that build; tools\sdk_diff.py
//           shows what moved between two dumps)
//
// How the 22284 values were obtained is recorded next to each one that was not a plain
// unique signature match; see port-nov15.md for the method and the evidence.
#pragma once
#include <cstdint>

#if !defined(HALCYON_GAME_BUILD)
#error "HALCYON_GAME_BUILD is not defined - build through HalcyonA2.vcxproj (it sets it from $(GameBuild))"
#endif

namespace HX
{
// ===========================================================================
// 5.4.2-20996  (Nov 2024 Rift build, A2-Win64-Shipping.exe 166,396,928 bytes)
// The original reference values from code.txt, unchanged.
// ===========================================================================
#if HALCYON_GAME_BUILD == 20996
    // --- engine globals (.data) ---
    constexpr uintptr_t GIsClient_RVA          = 0x9650702;
    constexpr uintptr_t GIsServer_RVA          = 0x9650703;
    constexpr uintptr_t StationStr_Data_RVA    = 0x95579C0;   // GA2StationId FString
    constexpr uintptr_t StationStr_Num_RVA     = 0x95579C8;
    constexpr uintptr_t StationStr_Max_RVA     = 0x95579CC;
    constexpr uintptr_t DashClientPtr_RVA      = 0x9BD44F0;   // qword: station-dashboard client
    constexpr uintptr_t LogA2StationDashboard_RVA = 0x9BD4450; // log category (byte 0 = verbosity)
    constexpr uintptr_t LogA2SessionSubsystem_RVA = 0x9BD4170;

    // --- hooked / called functions ---
    constexpr uintptr_t JwtBuild_RVA           = 0x53FD500;   // Vivox JWT assembler
    constexpr uintptr_t ReqLogin_RVA           = 0x53EF080;   // RequestVivoxLoginToken_Implementation
    constexpr uintptr_t JoinBuild_RVA          = 0x53E7730;   // Vivox join-token builder
    constexpr uintptr_t SendJoin_RVA           = 0x52AD8B0;   // ReceiveChannelJoinTokens sender
    constexpr uintptr_t SendPhysImpl_RVA       = 0x5496650;   // SendPhysicsPropData impl
    constexpr uintptr_t SimIntegrate_RVA       = 0x5434CE0;   // ball sim integrate
    constexpr uintptr_t IngestInput_RVA        = 0x540B010;   // ball sim input ingest
    constexpr uintptr_t SendResults_RVA        = 0x5309C40;   // Client_SendServerSimResults
    constexpr uintptr_t StepSim_RVA            = 0x543F2D0;   // ball sim step
    constexpr uintptr_t NotifyLeftArena_RVA    = 0x54962B0;   // Server_NotifyPlayerLeftArena
    constexpr uintptr_t GolfBallInCup_RVA      = 0x53986A0;
    constexpr uintptr_t GolfOverlap_RVA        = 0x53AD660;
    constexpr uintptr_t FNameResolve_RVA       = 0x114E210;   // FName->string (crash guard)
    constexpr uintptr_t TextLayoutLeaf_RVA     = 0x1EC13E0;   // Slate text-run layout leaf (crash guard)
    constexpr uintptr_t EvtDispatch_RVA        = 0x465F820;   // UNetEventsBridge dispatch
    constexpr uintptr_t GiveCheck_RVA          = 0x46F6180;   // ATicketManager::GiveAndCheckTicket
    constexpr uintptr_t FetchRoles_RVA         = 0x541CB00;   // A2Station__FetchUserRoles
    constexpr uintptr_t IsRunningSimulate_RVA  = 0x52DC906;   // UGameplayUtilityStatics::IsRunningSimulate impl
    constexpr uintptr_t WorldGetNetMode_RVA    = 0x4037D80;   // UWorld::GetNetMode
    constexpr uintptr_t A2GetNetMode_RVA       = 0x5473BC0;   // UA2NetworkUtilityBPFL::GetNetMode impl
    constexpr uintptr_t BallSimGetWorld_RVA    = 0x3500950;   // BallSimManager GetWorld
    constexpr uintptr_t GetPlayerPawn_RVA      = 0x3D1C150;
    constexpr uintptr_t PrintString_RVA        = 0x3A21120;   // UKismetSystemLibrary::PrintString
    constexpr uintptr_t DashboardInit_RVA      = 0x541D8A0;   // station-dashboard reporting init
    constexpr uintptr_t DashboardGate_RVA      = 0x541D8EF;   // `cmp eax,2` in DashboardInit -> xor eax,eax; nop
    constexpr uintptr_t NetVarReg_RVA          = 0x46A0DF0;   // netvar register (DefaultLODSettings rewrite)
    constexpr uintptr_t ObjBuild_RVA           = 0x46D77A0;   // per-ObjectPrefab netvar builder
    constexpr uintptr_t QInit_RVA              = 0x46851C0;   // quest trace hooks
    constexpr uintptr_t QSetQ_RVA              = 0x4685250;
    constexpr uintptr_t QReg_RVA               = 0x4680E50;
    constexpr uintptr_t QSetP_RVA              = 0x4685C30;
    constexpr uintptr_t StreamThunk_RVA        = 0x54ABCC0;   // texture-streaming thunk null-guard
    constexpr uintptr_t LogfNop_RVA            = 0x547380A;   // `call FMsg::Logf` in GetLocalPlayer_Implementation -> nop x5

    // --- ball-sim code cluster scanned by PatchBallSimNetModeChecks ---
    constexpr uintptr_t BallSimLo_RVA          = 0x5400000;
    constexpr uintptr_t BallSimHi_RVA          = 0x5480000;

    // --- struct offsets (Dumper-7 SDK 20996) ---
    constexpr uint32_t VRPawn_BallSimManager   = 0x1A58;
    constexpr uint32_t VRPawn_Entity           = 0x840;
    constexpr uint32_t VRPawn_PlayerIndex      = 0x1B48;
    constexpr uint32_t VRPawn_CurrentDisc      = 0x1130;      // currentDiscEntity
    constexpr uint32_t VRPawn_HeartBall        = 0x1DC0;
    constexpr uint32_t VRPawn_CurrentTicketMgr = 0x1128;      // CurrentTicketManager
    constexpr uint32_t VRPawn_GamemodeSlot     = 0x1C40;
    constexpr uint32_t VRPawn_SlotIdNum        = 0x1CB0;      // SlotID (FString) .Num
    constexpr uint32_t VRPawn_PawnCurrentTicket= 0x1CB8;

    // UA2PlayerEntity: two FReplicatedVRPlayerData copies (localData / VRPlayerRepData); each
    // holds FrequentData at +0 (Ping @+8), TeamIndex @+0x110, CurrentTeamColor @+0x1F8.
    constexpr uint32_t Ent_FreqLocal           = 0x0F0;       // localData.FrequentData
    constexpr uint32_t Ent_FreqRep             = 0x308;       // VRPlayerRepData.FrequentData (replicated)
    constexpr uint32_t Ent_TeamIdxLocal        = 0x200;
    constexpr uint32_t Ent_TeamIdxRep          = 0x418;
    constexpr uint32_t Ent_ColorLocal          = 0x2E8;
    constexpr uint32_t Ent_ColorRep            = 0x500;
    constexpr uint32_t TeamColor_Size          = 0x14;        // sizeof(FTeamColor); also the stride of TicketManager.TeamColors

    // UA2VOIPSubsystem config FStrings
    constexpr uint32_t Voip_Issuer             = 0x108;
    constexpr uint32_t Voip_Domain             = 0x118;
    constexpr uint32_t Voip_SigningKey         = 0x128;
    constexpr uint32_t Voip_Server             = 0x138;

// ===========================================================================
// 5.4.2-22284  ("Nov15" build, A2-Win64-Shipping.exe 166,994,432 bytes)
// Ported from 20996: tools\port_offsets.py signature matches, verified in IDA
// (ida\A2-Nov15.i64). Struct offsets from the 22284 Dumper-7 dump (tools\sdk_diff.py).
// ===========================================================================
#elif HALCYON_GAME_BUILD == 22284
    // --- engine globals (.data) --- (resolved via IDA xrefs from ported functions)
    constexpr uintptr_t GIsClient_RVA          = 0;   // TODO_IDA
    constexpr uintptr_t GIsServer_RVA          = 0;   // TODO_IDA
    constexpr uintptr_t StationStr_Data_RVA    = 0;   // TODO_IDA
    constexpr uintptr_t StationStr_Num_RVA     = 0;   // TODO_IDA
    constexpr uintptr_t StationStr_Max_RVA     = 0;   // TODO_IDA
    constexpr uintptr_t DashClientPtr_RVA      = 0;   // TODO_IDA
    constexpr uintptr_t LogA2StationDashboard_RVA = 0; // TODO_IDA
    constexpr uintptr_t LogA2SessionSubsystem_RVA = 0; // TODO_IDA

    // --- hooked / called functions --- (unique strict-signature matches unless noted)
    constexpr uintptr_t JwtBuild_RVA           = 0x544D9B0;
    constexpr uintptr_t ReqLogin_RVA           = 0x543C7B0;
    constexpr uintptr_t JoinBuild_RVA          = 0x5432A40;
    constexpr uintptr_t SendJoin_RVA           = 0x52EE2E0;
    constexpr uintptr_t SendPhysImpl_RVA       = 0x5503050;
    constexpr uintptr_t SimIntegrate_RVA       = 0;   // TODO_IDA - ball-sim code changed; no unique signature
    constexpr uintptr_t IngestInput_RVA        = 0x54599B0;
    constexpr uintptr_t SendResults_RVA        = 0x534A7F0;
    constexpr uintptr_t StepSim_RVA            = 0x54863C0;
    constexpr uintptr_t NotifyLeftArena_RVA    = 0x5502B00;   // loose signature (struct displacements changed)
    constexpr uintptr_t GolfBallInCup_RVA      = 0x53DC320;
    constexpr uintptr_t GolfOverlap_RVA        = 0x53F03B0;
    constexpr uintptr_t FNameResolve_RVA       = 0x114D690;
    constexpr uintptr_t TextLayoutLeaf_RVA     = 0x1EC07A0;
    constexpr uintptr_t EvtDispatch_RVA        = 0x466A240;
    constexpr uintptr_t GiveCheck_RVA          = 0x4723AA0;
    constexpr uintptr_t FetchRoles_RVA         = 0x54B04E0;
    constexpr uintptr_t IsRunningSimulate_RVA  = 0x531EF96;
    constexpr uintptr_t WorldGetNetMode_RVA    = 0x4040990;
    constexpr uintptr_t A2GetNetMode_RVA       = 0x54AF360;
    constexpr uintptr_t BallSimGetWorld_RVA    = 0x3509AB0;
    constexpr uintptr_t GetPlayerPawn_RVA      = 0x3D24C40;
    constexpr uintptr_t PrintString_RVA        = 0x3A29B20;
    constexpr uintptr_t DashboardInit_RVA      = 0x54B13D0;
    constexpr uintptr_t DashboardGate_RVA      = 0x54B141F;   // same +0x4F into DashboardInit as 20996
    constexpr uintptr_t NetVarReg_RVA          = 0;   // TODO_IDA - two candidates (0x46BC830 / 0x47138D0)
    constexpr uintptr_t ObjBuild_RVA           = 0x4716FE0;
    constexpr uintptr_t QInit_RVA              = 0x4690260;
    constexpr uintptr_t QSetQ_RVA              = 0x46902F0;
    constexpr uintptr_t QReg_RVA               = 0x468BE60;
    constexpr uintptr_t QSetP_RVA              = 0x46910D0;
    constexpr uintptr_t StreamThunk_RVA        = 0x550F130;
    constexpr uintptr_t LogfNop_RVA            = 0x54EAD9A;

    // --- ball-sim code cluster --- (TODO_IDA: re-derive around the new sim functions)
    constexpr uintptr_t BallSimLo_RVA          = 0;
    constexpr uintptr_t BallSimHi_RVA          = 0;

    // --- struct offsets (Dumper-7 SDK 22284) ---
    constexpr uint32_t VRPawn_BallSimManager   = 0x1B38;      // was 0x1A58
    constexpr uint32_t VRPawn_Entity           = 0x928;       // was 0x840
    constexpr uint32_t VRPawn_PlayerIndex      = 0x1C22;      // was 0x1B48 (uint8, now packed after two new bools)
    constexpr uint32_t VRPawn_CurrentDisc      = 0x1210;      // was 0x1130
    constexpr uint32_t VRPawn_HeartBall        = 0x1E60;      // was 0x1DC0
    constexpr uint32_t VRPawn_CurrentTicketMgr = 0x1208;      // was 0x1128
    constexpr uint32_t VRPawn_GamemodeSlot     = 0x1D18;      // was 0x1C40
    constexpr uint32_t VRPawn_SlotIdNum        = 0x1D88;      // SlotID @0x1D80 + 8
    constexpr uint32_t VRPawn_PawnCurrentTicket= 0x1D90;      // was 0x1CB8

    // UA2PlayerEntity was reworked: the two FReplicatedVRPlayerData copies are gone. There is
    // now ONE replicated FReplicatedFrequentData at 0xF0 (Ping @+8, same layout as before) and
    // the team/colour fields moved into FA2PlayerCosmeticsFragment at 0x200 (TeamIndex @+8,
    // CurrentTeamColor @+0xD0). "local" and "rep" therefore point at the same fields, which
    // makes the payload's dual writes idempotent rather than wrong.
    constexpr uint32_t Ent_FreqLocal           = 0x0F0;
    constexpr uint32_t Ent_FreqRep             = 0x0F0;
    constexpr uint32_t Ent_TeamIdxLocal        = 0x208;
    constexpr uint32_t Ent_TeamIdxRep          = 0x208;
    constexpr uint32_t Ent_ColorLocal          = 0x2D0;
    constexpr uint32_t Ent_ColorRep            = 0x2D0;
    constexpr uint32_t TeamColor_Size          = 0x1C;        // FTeamColor grew 0x14 -> 0x1C (RPC params shift with it)

    // UA2VOIPSubsystem config FStrings (reordered in this build)
    constexpr uint32_t Voip_Issuer             = 0x0F0;       // was 0x108
    constexpr uint32_t Voip_Domain             = 0x0D0;       // was 0x118
    constexpr uint32_t Voip_SigningKey         = 0x138;       // was 0x128
    constexpr uint32_t Voip_Server             = 0x128;       // was 0x138

#else
#error "HALCYON_GAME_BUILD has no block in GameOffsets.h - add one for this build"
#endif

    constexpr int GameBuild = HALCYON_GAME_BUILD;
}
