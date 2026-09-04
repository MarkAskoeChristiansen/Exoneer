// Copyright Exoneer contributors.
#include "Misc/AutomationTest.h"
#include "Components/BuildToolComponent.h"
#include "Components/InventoryComponent.h"
#include "Data/VehicleBlockDefinitionDataAsset.h"
#include "Data/ItemDefinitionDataAsset.h"
#include "Interfaces/Constructible.h"
#include "Vehicles/VehicleConstruct.h"
#include "Vehicles/WheelModule.h"
#include "Engine/AssetManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * A transient game world for one test, torn down on scope exit so an early
	 * return cannot leak it. Same shape as the maintenance suite helper; the
	 * build-tool suite keeps its own so the two files stay independent.
	 */
	struct FExoneerScopedBuildToolWorld
	{
		UWorld* World = nullptr;

		FExoneerScopedBuildToolWorld()
		{
			if (!GEngine)
			{
				return;
			}
			World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false);
			if (!World)
			{
				return;
			}
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
			if (AWorldSettings* Settings = World->GetWorldSettings())
			{
				Settings->NotifyBeginPlay();
			}
		}

		~FExoneerScopedBuildToolWorld()
		{
			if (!World)
			{
				return;
			}
			World->EndPlay(EEndPlayReason::Quit);
			if (GEngine)
			{
				GEngine->DestroyWorldContext(World);
			}
			World->DestroyWorld(/*bInformEngineOfWorld*/ false);
			World = nullptr;
		}

		FExoneerScopedBuildToolWorld(const FExoneerScopedBuildToolWorld&) = delete;
		FExoneerScopedBuildToolWorld& operator=(const FExoneerScopedBuildToolWorld&) = delete;
	};

	/** A wheel block shaped like the bootstrap one, with an authored weld plan. */
	UVehicleBlockDefinitionDataAsset* MakeWeldableWheelDef(const TArray<float>& StageWork)
	{
		UVehicleBlockDefinitionDataAsset* Def = NewObject<UVehicleBlockDefinitionDataAsset>(GetTransientPackage());
		Def->BlockId = TEXT("test_weld_wheel");
		Def->SizeInCells = FIntVector(1, 1, 1);
		Def->Mass = 60.f;
		Def->MaxHealth = 250.f;
		Def->ModuleClass = UWheelModule::StaticClass();
		Def->bIsWheel = true;
		Def->bAllowTerrainOverlapOnPlace = true;
		Def->SpareItemId = TEXT("tire");
		Def->Stages.Reset();
		for (const float Work : StageWork)
		{
			// No materials on the stages: this suite is about the weld RULE,
			// not about the stock ledger, which the maintenance suite covers.
			FConstructionCost Stage;
			Stage.WeldWork = Work;
			Def->Stages.Add(Stage);
		}
		return Def;
	}

	/** The welder stand-in: a tool and a pack, inside weld reach of the origin. */
	struct FTestWelder
	{
		AActor* Actor = nullptr;
		UBuildToolComponent* Tool = nullptr;
		UInventoryComponent* Inventory = nullptr;
	};

	FTestWelder MakeWelder(UWorld* World)
	{
		FTestWelder Welder;
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Welder.Actor = World->SpawnActor<AActor>(AActor::StaticClass(),
			FTransform(FVector(100.f, 0.f, 0.f)), SpawnParams);
		if (!Welder.Actor)
		{
			return Welder;
		}
		Welder.Tool = NewObject<UBuildToolComponent>(Welder.Actor);
		Welder.Tool->RegisterComponent();
		Welder.Inventory = NewObject<UInventoryComponent>(Welder.Actor);
		Welder.Inventory->RegisterComponent();
		return Welder;
	}

	UItemDefinitionDataAsset* ResolveSpare(FName SpareId)
	{
		const FPrimaryAssetId SpareAsset(TEXT("Item"), SpareId);
		UItemDefinitionDataAsset* Spare = Cast<UItemDefinitionDataAsset>(
			UAssetManager::Get().GetPrimaryAssetObject(SpareAsset));
		if (!Spare)
		{
			TSoftObjectPtr<UItemDefinitionDataAsset> Soft(UAssetManager::Get().GetPrimaryAssetPath(SpareAsset));
			Spare = Soft.LoadSynchronous();
		}
		return Spare;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerHeldWeldKeepsFinishedWork,
	"Exoneer.BuildTool.HeldWeldKeepsFinishedWork",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExoneerHeldWeldKeepsFinishedWork::RunTest(const FString& Parameters)
{
	// The reported bug: holding the beam a moment past completion destroyed the
	// block that had just been finished. The rule is that a held stream can
	// only ever INVEST - once the part is Complete the rest of that hold does
	// nothing at all - so this drives a whole hold through the real server
	// routing and then checks the block is still there, whole and untouched.
	FExoneerScopedBuildToolWorld Scoped;
	UWorld* World = Scoped.World;
	if (!World)
	{
		AddError(TEXT("could not create a transient game world"));
		return false;
	}

	const float StageWork = 20.f;
	UVehicleBlockDefinitionDataAsset* Def = MakeWeldableWheelDef({ StageWork });

	EBuildPlacementError PlacementError = EBuildPlacementError::None;
	AVehicleConstruct* Construct = AVehicleConstruct::FoundConstruct(
		World, Def, FTransform(FVector::ZeroVector), PlacementError, 0);
	if (!Construct || Construct->GetBlocks().Num() != 1)
	{
		AddError(FString::Printf(TEXT("FoundConstruct failed, placement error %d"), static_cast<int32>(PlacementError)));
		return false;
	}
	const int32 BlockId = Construct->GetBlocks()[0].BlockInstanceId;

	// A fresh ghost, and a SCRAP tire on it: a terminal tread reading is the
	// one condition that makes ReplacePartAt legal, so if any batch of the hold
	// reached the maintenance verbs it would spend a spare and show up below.
	TestTrue(TEXT("block restored to a fresh ghost"),
		Construct->RestoreBlockRecord(BlockId, EConstructionPhase::Ghost, 0, 0.f, 0.f, 0.f));
	FPartCondition Scrap;
	Scrap.TreadDepthMm = 0.f;
	Scrap.InflationKPa = 0.f;
	TestTrue(TEXT("scrap tire condition restored"), Construct->RestoreBlockCondition(BlockId, Scrap));

	FTestWelder Welder = MakeWelder(World);
	if (!Welder.Tool)
	{
		AddError(TEXT("could not spawn the welder actor"));
		return false;
	}
	UItemDefinitionDataAsset* Spare = ResolveSpare(Def->SpareItemId);
	if (!Spare)
	{
		AddError(TEXT("could not resolve the authored tire item (Item:tire)"));
		return false;
	}
	TestEqual(TEXT("two spares fit in the pack"), Welder.Inventory->AddItem(Spare, 2), 0);

	const FVector WeldPoint = Construct->CellToWorld(FIntVector::ZeroValue);
	const float BatchPoints = 5.f;

	// One press: the first batch is the fresh one, every later batch is the
	// tail of the same hold. Four batches finish the 20 point stage; the hold
	// runs on for another twenty, which is the overhold that deleted the work.
	Welder.Tool->ServerApplyWeld(Construct, WeldPoint, BatchPoints, /*bFreshPress*/ true);
	TestEqual(TEXT("the first quarter of the stage reports 25 percent"),
		Welder.Tool->LastWeldProgress01, 0.25f);

	for (int32 Batch = 0; Batch < 23; ++Batch)
	{
		Welder.Tool->ServerApplyWeld(Construct, WeldPoint, BatchPoints, /*bFreshPress*/ false);
	}

	const FVehicleBlockRecord* After = Construct->FindRecord(BlockId);
	if (!After)
	{
		AddError(TEXT("the held weld REMOVED the block it had just finished"));
		return false;
	}
	TestEqual(TEXT("the block is still on the grid"), Construct->GetBlocks().Num(), 1);
	TestEqual(TEXT("the held weld left the block Complete"),
		static_cast<int32>(After->Phase), static_cast<int32>(EConstructionPhase::Complete));
	TestEqual(TEXT("completing the weld set full health"), After->Health, Def->MaxHealth);
	TestEqual(TEXT("the tail of the hold spent no spare"), Welder.Inventory->GetItemCount(Spare), 2);
	TestEqual(TEXT("the tail of the hold replaced no tire"), After->Condition.TreadDepthMm, 0.f);
	TestEqual(TEXT("the tail of the hold reports the target complete"),
		static_cast<int32>(Welder.Tool->LastWeldResult), 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerFreshPressOnCompleteRoutesTheVerb,
	"Exoneer.BuildTool.FreshPressOnCompleteRoutesTheVerb",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExoneerFreshPressOnCompleteRoutesTheVerb::RunTest(const FString& Parameters)
{
	// The other half of the rule: the maintenance verbs are not gone, they are
	// gated. A deliberate new press on an already-Complete part still routes to
	// the verb its reading calls for - here a scrap tire takes a spare.
	FExoneerScopedBuildToolWorld Scoped;
	UWorld* World = Scoped.World;
	if (!World)
	{
		AddError(TEXT("could not create a transient game world"));
		return false;
	}

	UVehicleBlockDefinitionDataAsset* Def = MakeWeldableWheelDef({ 20.f });
	const FVehicleWheelSpec& Wheel = Def->WheelSpec;

	EBuildPlacementError PlacementError = EBuildPlacementError::None;
	AVehicleConstruct* Construct = AVehicleConstruct::FoundConstruct(
		World, Def, FTransform(FVector::ZeroVector), PlacementError, 0);
	if (!Construct || Construct->GetBlocks().Num() != 1)
	{
		AddError(FString::Printf(TEXT("FoundConstruct failed, placement error %d"), static_cast<int32>(PlacementError)));
		return false;
	}
	const int32 BlockId = Construct->GetBlocks()[0].BlockInstanceId;
	TestTrue(TEXT("block restored Complete"),
		Construct->RestoreBlockRecord(BlockId, EConstructionPhase::Complete, 0, 1.f, Def->MaxHealth, 0.f));
	FPartCondition Scrap;
	Scrap.TreadDepthMm = 0.f;
	Scrap.InflationKPa = 0.f;
	TestTrue(TEXT("scrap tire condition restored"), Construct->RestoreBlockCondition(BlockId, Scrap));

	FTestWelder Welder = MakeWelder(World);
	if (!Welder.Tool)
	{
		AddError(TEXT("could not spawn the welder actor"));
		return false;
	}
	UItemDefinitionDataAsset* Spare = ResolveSpare(Def->SpareItemId);
	if (!Spare)
	{
		AddError(TEXT("could not resolve the authored tire item (Item:tire)"));
		return false;
	}
	TestEqual(TEXT("two spares fit in the pack"), Welder.Inventory->AddItem(Spare, 2), 0);

	const FVector WeldPoint = Construct->CellToWorld(FIntVector::ZeroValue);

	// Tail of a hold: refused, whatever the reading says.
	Welder.Tool->ServerApplyWeld(Construct, WeldPoint, 5.f, /*bFreshPress*/ false);
	TestEqual(TEXT("a held batch spends no spare"), Welder.Inventory->GetItemCount(Spare), 2);
	TestEqual(TEXT("a held batch reports the target complete"),
		static_cast<int32>(Welder.Tool->LastWeldResult), 3);

	// Deliberate new press: the verb runs.
	Welder.Tool->ServerApplyWeld(Construct, WeldPoint, 5.f, /*bFreshPress*/ true);
	const FVehicleBlockRecord* Replaced = Construct->FindRecord(BlockId);
	if (!Replaced)
	{
		AddError(TEXT("the replace press removed the block"));
		return false;
	}
	TestEqual(TEXT("the fresh press spends exactly one spare"), Welder.Inventory->GetItemCount(Spare), 1);
	TestEqual(TEXT("the fresh press reports a swap"),
		static_cast<int32>(Welder.Tool->LastWeldResult), 6);
	TestEqual(TEXT("the new tire is at full tread"), Replaced->Condition.TreadDepthMm, Wheel.NewTreadDepthMm);
	TestEqual(TEXT("the block survived the swap"), Construct->GetBlocks().Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerWeldProgressIsTheTrueFraction,
	"Exoneer.BuildTool.WeldProgressIsTheTrueFraction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExoneerWeldProgressIsTheTrueFraction::RunTest(const FString& Parameters)
{
	// The readout is build progress: the fraction of the authored weld work
	// this part has taken, across ALL its stages. It must be the true fraction
	// at every sample, including part way through a stage, so a per-frame
	// reader counts every value from 0 to 100 instead of stepping.
	FExoneerScopedBuildToolWorld Scoped;
	UWorld* World = Scoped.World;
	if (!World)
	{
		AddError(TEXT("could not create a transient game world"));
		return false;
	}

	// Two uneven stages, 10 and 30 weld points: a stage-granular readout would
	// report 0, 50, 100 here, and a stage-local one would report the wrong
	// number as soon as the second stage started.
	UVehicleBlockDefinitionDataAsset* Def = MakeWeldableWheelDef({ 10.f, 30.f });
	const float TotalWork = 40.f;

	EBuildPlacementError PlacementError = EBuildPlacementError::None;
	AVehicleConstruct* Construct = AVehicleConstruct::FoundConstruct(
		World, Def, FTransform(FVector::ZeroVector), PlacementError, 0);
	if (!Construct || Construct->GetBlocks().Num() != 1)
	{
		AddError(FString::Printf(TEXT("FoundConstruct failed, placement error %d"), static_cast<int32>(PlacementError)));
		return false;
	}
	const int32 BlockId = Construct->GetBlocks()[0].BlockInstanceId;
	TestTrue(TEXT("block restored to a fresh ghost"),
		Construct->RestoreBlockRecord(BlockId, EConstructionPhase::Ghost, 0, 0.f, 0.f, 0.f));

	FTestWelder Welder = MakeWelder(World);
	if (!Welder.Tool)
	{
		AddError(TEXT("could not spawn the welder actor"));
		return false;
	}

	const FVector WeldPoint = Construct->CellToWorld(FIntVector::ZeroValue);

	// 4 points lands part way through stage 0; 12 lands part way through stage
	// 1, which is where a stage-local fraction would report 2/30, not 12/40.
	// Every batch stays inside the one-second budget the server clamps to, so
	// the running total below is what actually went in.
	const float Steps[] = { 4.f, 4.f, 4.f, 8.f, 10.f, 10.f };
	float Invested = 0.f;
	for (const float Points : Steps)
	{
		Welder.Tool->ServerApplyWeld(Construct, WeldPoint, Points, /*bFreshPress*/ false);
		Invested = FMath::Min(Invested + Points, TotalWork);
		const float Expected = Invested / TotalWork;

		const float Reported = IConstructible::Execute_GetConstructionProgressAt(Construct, WeldPoint);
		TestTrue(FString::Printf(TEXT("after %.0f points the target reports %.4f, expected %.4f"),
			Invested, Reported, Expected), FMath::IsNearlyEqual(Reported, Expected, 1e-3f));
		TestTrue(FString::Printf(TEXT("after %.0f points the HUD feedback carries %.4f, expected %.4f"),
			Invested, Welder.Tool->LastWeldProgress01, Expected),
			FMath::IsNearlyEqual(Welder.Tool->LastWeldProgress01, Expected, 1e-3f));
	}

	const FVehicleBlockRecord* Done = Construct->FindRecord(BlockId);
	if (!Done)
	{
		AddError(TEXT("the welded block vanished"));
		return false;
	}
	TestEqual(TEXT("the last weld point completes the block"),
		static_cast<int32>(Done->Phase), static_cast<int32>(EConstructionPhase::Complete));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerWeldProgressFollowsTheBlockTakingTheWork,
	"Exoneer.BuildTool.WeldProgressFollowsTheBlockTakingTheWork",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExoneerWeldProgressFollowsTheBlockTakingTheWork::RunTest(const FString& Parameters)
{
	// The reported bug: welding a ghost block NEXT TO a finished one read 100
	// percent long before the ghost was done. The weld aim sweep is 14 cm and
	// a block is 25 cm, so the point that resolves the target routinely lands
	// on the finished neighbour - and a point-addressed progress query answers
	// 1.0 for a Complete record and for a clean miss alike. The weld now
	// reports WHICH record took the work and the readout echoes that record,
	// so the number can only ever describe the block being welded.
	FExoneerScopedBuildToolWorld Scoped;
	UWorld* World = Scoped.World;
	if (!World)
	{
		AddError(TEXT("could not create a transient game world"));
		return false;
	}

	UVehicleBlockDefinitionDataAsset* Def = MakeWeldableWheelDef({ 10.f, 30.f });
	const float TotalWork = 40.f;

	EBuildPlacementError PlacementError = EBuildPlacementError::None;
	AVehicleConstruct* Construct = AVehicleConstruct::FoundConstruct(
		World, Def, FTransform(FVector::ZeroVector), PlacementError, 0);
	if (!Construct || Construct->GetBlocks().Num() != 1)
	{
		AddError(FString::Printf(TEXT("FoundConstruct failed, placement error %d"), static_cast<int32>(PlacementError)));
		return false;
	}

	// The finished neighbour, and the ghost one cell along that the engineer
	// is actually aiming at.
	const int32 FinishedId = Construct->GetBlocks()[0].BlockInstanceId;
	TestTrue(TEXT("the neighbour is welded whole"),
		Construct->RestoreBlockRecord(FinishedId, EConstructionPhase::Complete, 1, 1.f, Def->MaxHealth, 0.f));

	const int32 GhostId = Construct->PlaceBlockGhost(Def, FIntVector(1, 0, 0), 0);
	if (GhostId == INDEX_NONE)
	{
		AddError(TEXT("could not place the adjacent ghost block"));
		return false;
	}

	FTestWelder Welder = MakeWelder(World);
	if (!Welder.Tool)
	{
		AddError(TEXT("could not spawn the welder actor"));
		return false;
	}

	// Aim squarely at the FINISHED block: this is the sweep landing on the
	// neighbour, which is the whole failure mode.
	const FVector WeldPoint = Construct->CellToWorld(FIntVector::ZeroValue);
	TestEqual(TEXT("the aim point really does resolve to the finished block"),
		Construct->FindBlockAtWorldPoint(WeldPoint), FinishedId);
	TestEqual(TEXT("and a point-addressed query really does answer 100 percent there"),
		IConstructible::Execute_GetConstructionProgressAt(Construct, WeldPoint), 1.f);

	const float Steps[] = { 4.f, 4.f, 4.f, 8.f, 10.f, 10.f };
	float Invested = 0.f;
	for (const float Points : Steps)
	{
		Welder.Tool->ServerApplyWeld(Construct, WeldPoint, Points, /*bFreshPress*/ false);
		Invested = FMath::Min(Invested + Points, TotalWork);
		const float Expected = Invested / TotalWork;
		const bool bGhostDone = Invested >= TotalWork;

		TestEqual(TEXT("the work is reported against the ghost, not the neighbour"),
			Welder.Tool->LastWeldTargetId, GhostId);
		TestEqual(TEXT("the weld reports progress"),
			static_cast<int32>(Welder.Tool->LastWeldResult), 0);
		TestTrue(FString::Printf(TEXT("after %.0f points the HUD feedback carries %.4f, expected %.4f"),
			Invested, Welder.Tool->LastWeldProgress01, Expected),
			FMath::IsNearlyEqual(Welder.Tool->LastWeldProgress01, Expected, 1e-3f));

		// The live per-frame sample reads the SAME identity off the replicated
		// state, so it agrees with the echo at every sample.
		const float Live = IConstructible::Execute_GetConstructionProgressForTarget(Construct, Welder.Tool->LastWeldTargetId);
		TestTrue(FString::Printf(TEXT("the live sample reads %.4f, expected %.4f"), Live, Expected),
			FMath::IsNearlyEqual(Live, Expected, 1e-3f));

		// THE PIN: not 100 percent until the ghost itself is Complete.
		if (!bGhostDone)
		{
			TestTrue(FString::Printf(TEXT("the echo must not read 100 percent at %.0f of %.0f points"), Invested, TotalWork),
				Welder.Tool->LastWeldProgress01 < 1.f - KINDA_SMALL_NUMBER);
			TestTrue(FString::Printf(TEXT("the live sample must not read 100 percent at %.0f of %.0f points"), Invested, TotalWork),
				Live < 1.f - KINDA_SMALL_NUMBER);
			const FVehicleBlockRecord* Ghost = Construct->FindRecord(GhostId);
			TestTrue(TEXT("the ghost is not Complete yet"),
				Ghost && Ghost->Phase != EConstructionPhase::Complete);
		}
	}

	const FVehicleBlockRecord* Ghost = Construct->FindRecord(GhostId);
	if (!Ghost)
	{
		AddError(TEXT("the welded ghost vanished"));
		return false;
	}
	TestEqual(TEXT("the last weld point completes the ghost"),
		static_cast<int32>(Ghost->Phase), static_cast<int32>(EConstructionPhase::Complete));
	TestEqual(TEXT("only then does the readout reach 100 percent"),
		Welder.Tool->LastWeldProgress01, 1.f);
	TestEqual(TEXT("the finished neighbour was never touched"),
		static_cast<int32>(Construct->FindRecord(FinishedId)->Phase), static_cast<int32>(EConstructionPhase::Complete));

	// An identity from nowhere is UNKNOWN, never finished: that is what stops
	// a miss from painting 100 percent on the visor.
	TestTrue(TEXT("an unknown identity reports no progress at all"),
		IConstructible::Execute_GetConstructionProgressForTarget(Construct, 987654) < 0.f);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
