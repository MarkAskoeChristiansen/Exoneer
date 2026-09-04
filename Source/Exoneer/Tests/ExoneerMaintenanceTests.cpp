// Copyright Exoneer contributors.
#include "Misc/AutomationTest.h"
#include "Maintenance/ExoneerMaintenance.h"
#include "Vehicles/ExoneerTerramechanics.h"
#include "Vehicles/VehicleWheelSpec.h"
#include "Vehicles/VehicleConstruct.h"
#include "Vehicles/WheelModule.h"
#include "Data/VehicleBlockDefinitionDataAsset.h"
#include "Components/BuildToolComponent.h"
#include "Components/InventoryComponent.h"
#include "Components/SurvivalStatsComponent.h"
#include "Components/ConstructionComponent.h"
#include "Building/BasePiece.h"
#include "Building/BaseStructure.h"
#include "Machines/BatteryPiece.h"
#include "Components/PowerComponent.h"
#include "Data/PieceDefinitionDataAsset.h"
#include "ExoneerGameplayTags.h"
#include "Data/ItemDefinitionDataAsset.h"
#include "Engine/AssetManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerTreadWearMonotone,
	"Exoneer.Maintenance.TreadWearMonotone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExoneerTreadWearMonotone::RunTest(const FString& Parameters)
{
	using namespace ExoneerMaintenance;
	const float Dt = 1.f;
	// Wear is frictional WORK in the patch: shear force times sliding speed.
	// A patch that carries force without sliding does no work and loses no
	// rubber, which is why rolling on firm ground is nearly free.
	TestTrue(TEXT("more work wears more"), TreadWearMm(2000.f * Dt) > TreadWearMm(500.f * Dt));
	TestTrue(TEXT("no sliding, no wear"), TreadWearMm(0.f) == 0.f);
	TestTrue(TEXT("wear is linear in work"),
		FMath::IsNearlyEqual(TreadWearMm(2000.f), 2.f * TreadWearMm(1000.f), 1e-9f));

	// A pure sideways plow does work through the LATERAL channel, so it must
	// wear even at zero longitudinal slip - the old |s| * W law wore nothing
	// there, and wore the same at 0.5 m/s as at 16 m/s.
	const float PlowW = 1200.f * 4.f;   // 1.2 kN of scrub sliding at 4 m/s
	TestTrue(TEXT("lateral scrub wears the tread"), TreadWearMm(PlowW * Dt) > 0.f);

	// Calibration anchors, from the measured operating points of the 6x6 test
	// rover. A bogged wheel spinning at full slip burns roughly 9 kW in the
	// patch and must scrap a new tire inside an hour; cruising on soft soil at
	// 1-3 kW must take hours, not the ninety seconds the slip-and-load law gave.
	auto SecondsToScrap = [](float PatchPowerW)
	{
		float Tread = DefaultNewTreadMm;
		int32 Seconds = 0;
		while (Tread > 0.f && Seconds < 200000)
		{
			Tread -= TreadWearMm(PatchPowerW * 1.f);
			++Seconds;
		}
		return Seconds;
	};
	const int32 BoggedS = SecondsToScrap(9000.f);
	const int32 CruiseS = SecondsToScrap(1700.f);
	TestTrue(TEXT("a bogged spinning wheel is scrap within the hour"), BoggedS < 3600);
	TestTrue(TEXT("but not within a single minute"), BoggedS > 600);
	TestTrue(TEXT("soft-soil cruising lasts hours"), CruiseS > 3600);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerTreadMobilisation,
	"Exoneer.Maintenance.TreadMobilisationScale",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExoneerTreadMobilisation::RunTest(const FString& Parameters)
{
	using namespace ExoneerMaintenance;
	TestEqual(TEXT("new tire full scale"), TreadMobilisationScale(12.f, 12.f), 1.f);
	TestTrue(TEXT("bald tire near floor"), TreadMobilisationScale(0.f, 12.f) <= 0.06f);
	TestTrue(TEXT("half tread is half scale"), FMath::IsNearlyEqual(TreadMobilisationScale(6.f, 12.f), 0.5f, 0.01f));

	// The terminal reading is the mobilisation floor: the point where more wear
	// buys the driver nothing, so the replace verb becomes legal.
	TestFalse(TEXT("a new tire is not terminal"), IsTreadTerminal(12.f, 12.f));
	TestFalse(TEXT("a half worn tire is not terminal"), IsTreadTerminal(6.f, 12.f));
	TestFalse(TEXT("2 mm of tread is still a tire"), IsTreadTerminal(2.f, 12.f));
	TestTrue(TEXT("at the mobilisation floor the tire is scrap rubber"),
		IsTreadTerminal(TreadMobilisationFloor * 12.f, 12.f));
	TestTrue(TEXT("bald is terminal"), IsTreadTerminal(0.f, 12.f));
	TestFalse(TEXT("a part with no tread reading is never terminal for tread"), IsTreadTerminal(-1.f, 12.f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerDustOpacity,
	"Exoneer.Maintenance.DustOpacity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExoneerDustOpacity::RunTest(const FString& Parameters)
{
	using namespace ExoneerMaintenance;
	float Op = 0.f;
	for (int32 i = 0; i < 50; ++i)
	{
		Op = FMath::Clamp(Op + DustOpacityDelta(1.f, 1.f), 0.f, 1.f);
	}
	TestTrue(TEXT("full storm ~50s reaches opaque"), Op >= 0.99f);
	TestEqual(TEXT("no storm no dust"), DustOpacityDelta(0.f, 1.f), 0.f);
	return true;
}

namespace
{
	/** The authored wheel thermal constants, so the tests pin content, not literals. */
	const FVehicleWheelSpec& ThermalSpec()
	{
		static const FVehicleWheelSpec Spec;
		return Spec;
	}

	/** Run the lumped thermal model to steady state from ambient. */
	float SettleWindingTempC(float LossW, float AmbientC, const FVehicleWheelSpec& Spec, float Seconds = 3600.f)
	{
		const float Dt = 0.05f;
		const int32 Steps = FMath::RoundToInt(Seconds / Dt);
		float TempC = AmbientC;
		for (int32 Step = 0; Step < Steps; ++Step)
		{
			TempC = ExoneerMaintenance::WindingTempStep(TempC, LossW, AmbientC,
				Spec.CoolingWPerC, Spec.ThermalMassJPerC, Dt);
		}
		return TempC;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerMotorLossVsDerate,
	"Exoneer.Maintenance.MotorLossVsDerate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExoneerMotorLossVsDerate::RunTest(const FString& Parameters)
{
	using namespace ExoneerMaintenance;
	const FVehicleWheelSpec& Spec = ThermalSpec();

	// Heat is LOSS, never shaft work: a motor holding 100 Nm at 20 rad/s does
	// 2 kW of useful work and only heats with copper + efficiency shortfall.
	const float CruiseTorque = 100.f;
	const float CruiseOmega = 20.f;
	const float CruiseLossW = MotorLossPowerW(CruiseTorque, CruiseOmega, Spec.MaxMotorTorqueNm,
		Spec.DrivetrainEfficiency, Spec.CopperLossAtStallW, Spec.ControllerIdleDrawW);
	const float ExpectedCopperW = Spec.CopperLossAtStallW * FMath::Square(CruiseTorque / Spec.MaxMotorTorqueNm);
	const float ExpectedFrictionW = (1.f - Spec.DrivetrainEfficiency) * CruiseTorque * CruiseOmega;
	TestTrue(TEXT("cruise loss is copper + friction + idle only"),
		FMath::IsNearlyEqual(CruiseLossW, ExpectedCopperW + ExpectedFrictionW + Spec.ControllerIdleDrawW, 1.f));
	TestTrue(TEXT("cruise loss is far below the 2 kW of shaft work"), CruiseLossW < 1000.f);

	// The derate-invariance contract: the copper basis is the DERATED stall
	// torque, so the ratio T/T_s stays 1 at stall and a hot motor keeps
	// burning its full copper loss instead of cooling itself by derating.
	const float TestTempsC[] = { 20.f, 130.f, 145.f, 200.f };
	float PreviousStallLossW = -1.f;
	for (const float TempC : TestTempsC)
	{
		const float Scale = ThermalDerateScale(TempC, Spec.DerateOnsetTempC, Spec.TripTempC);
		const float StallTorque = Spec.MaxMotorTorqueNm * Scale;
		// Full throttle against stuck ground: omega 0, so drive torque = stall.
		const float DriveTorque = ExoneerTerramechanics::MotorTorque(StallTorque, Spec.NoLoadSpeedRadS, 0.f, 1.f, 1.f);
		TestTrue(TEXT("stalled motor makes its full derated torque"),
			FMath::IsNearlyEqual(DriveTorque, StallTorque, 0.01f));
		const float LossW = MotorLossPowerW(DriveTorque, 0.f, StallTorque,
			Spec.DrivetrainEfficiency, Spec.CopperLossAtStallW, Spec.ControllerIdleDrawW);
		TestTrue(TEXT("stall loss holds the full copper loss as the derate bites"),
			FMath::IsNearlyEqual(LossW, Spec.CopperLossAtStallW + Spec.ControllerIdleDrawW, 1.f));
		if (PreviousStallLossW >= 0.f)
		{
			TestTrue(TEXT("stall loss never falls with the derate scale"), LossW >= PreviousStallLossW - 0.01f);
		}
		PreviousStallLossW = LossW;
	}

	// Cut out: zero stall torque means zero torque and no loss but the
	// controller draw - the guarded denominator, not a divide by zero.
	TestEqual(TEXT("a cut-out motor makes no torque"),
		ExoneerTerramechanics::MotorTorque(0.f, Spec.NoLoadSpeedRadS, 0.f, 1.f, 1.f), 0.f);
	TestEqual(TEXT("a cut-out motor only burns controller draw"),
		MotorLossPowerW(0.f, 10.f, 0.f, Spec.DrivetrainEfficiency, Spec.CopperLossAtStallW, Spec.ControllerIdleDrawW),
		Spec.ControllerIdleDrawW);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerWindingThermalEquilibria,
	"Exoneer.Maintenance.WindingThermalEquilibria",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExoneerWindingThermalEquilibria::RunTest(const FString& Parameters)
{
	using namespace ExoneerMaintenance;
	const FVehicleWheelSpec& Spec = ThermalSpec();
	const float AmbientC = 20.f;

	// Case 1, bogged at full stall: ~5 kW of copper loss must settle ABOVE the
	// trip temperature, or the cutout is unreachable and the whole class is
	// decorative. 20 + 5020 / 28 = 199 C.
	const float StallLossW = MotorLossPowerW(Spec.MaxMotorTorqueNm, 0.f, Spec.MaxMotorTorqueNm,
		Spec.DrivetrainEfficiency, Spec.CopperLossAtStallW, Spec.ControllerIdleDrawW);
	const float StallEquilibriumC = SettleWindingTempC(StallLossW, AmbientC, Spec);
	TestTrue(TEXT("stall equilibrium is the analytic ambient + loss / cooling"),
		FMath::IsNearlyEqual(StallEquilibriumC, AmbientC + StallLossW / Spec.CoolingWPerC, 0.5f));
	TestTrue(TEXT("stall equilibrium reaches ~199 C"),
		FMath::IsNearlyEqual(StallEquilibriumC, 199.f, 1.f));
	TestTrue(TEXT("stall equilibrium is above the trip, so the cutout is reachable"),
		StallEquilibriumC > Spec.TripTempC);
	TestTrue(TEXT("the clamp ceiling leaves the stall equilibrium alone"),
		StallEquilibriumC < Spec.TripTempC + 60.f);

	// Case 2, cruising at a third of stall torque: ~876 W must settle BELOW
	// the derate onset, or ordinary driving fades for no reason. ~51 C.
	const float CruiseLossW = MotorLossPowerW(100.f, 20.f, Spec.MaxMotorTorqueNm,
		Spec.DrivetrainEfficiency, Spec.CopperLossAtStallW, Spec.ControllerIdleDrawW);
	const float CruiseEquilibriumC = SettleWindingTempC(CruiseLossW, AmbientC, Spec);
	TestTrue(TEXT("cruise equilibrium reaches ~51 C"),
		FMath::IsNearlyEqual(CruiseEquilibriumC, 51.f, 1.5f));
	TestTrue(TEXT("cruise equilibrium is below the derate onset"),
		CruiseEquilibriumC < Spec.DerateOnsetTempC);

	// Cooling is toward ambient, not a hardcoded floor: a cold night pulls the
	// winding below 20 C, and a tripped motor clears in a bounded time.
	const float NightAmbientC = -40.f;
	TestTrue(TEXT("an idle winding cools to ambient, not to 20 C"),
		FMath::IsNearlyEqual(SettleWindingTempC(0.f, NightAmbientC, Spec), NightAmbientC, 0.5f));
	float TempC = Spec.TripTempC;
	float Seconds = 0.f;
	while (TempC > Spec.CutoutClearTempC && Seconds < 600.f)
	{
		TempC = WindingTempStep(TempC, Spec.ControllerIdleDrawW, AmbientC,
			Spec.CoolingWPerC, Spec.ThermalMassJPerC, 0.1f);
		Seconds += 0.1f;
	}
	TestTrue(TEXT("a tripped motor cools back to its clear temperature in well under a minute"),
		Seconds > 5.f && Seconds < 60.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerThermalDerateAndCutout,
	"Exoneer.Maintenance.ThermalDerateAndCutout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExoneerThermalDerateAndCutout::RunTest(const FString& Parameters)
{
	using namespace ExoneerMaintenance;
	const FVehicleWheelSpec& Spec = ThermalSpec();
	const float Onset = Spec.DerateOnsetTempC;
	const float Trip = Spec.TripTempC;

	TestEqual(TEXT("cold winding is not derated"), ThermalDerateScale(20.f, Onset, Trip), 1.f);
	TestEqual(TEXT("at the onset there is no derate yet"), ThermalDerateScale(Onset, Onset, Trip), 1.f);
	TestTrue(TEXT("halfway to the trip is halfway to the floor"),
		FMath::IsNearlyEqual(ThermalDerateScale(0.5f * (Onset + Trip), Onset, Trip),
			0.5f * (1.f + ThermalDerateFloor), 0.001f));
	TestEqual(TEXT("at the trip the derate is at its floor"),
		ThermalDerateScale(Trip, Onset, Trip), ThermalDerateFloor);
	TestEqual(TEXT("past the trip it stays at the floor, never 0"),
		ThermalDerateScale(Trip + 60.f, Onset, Trip), ThermalDerateFloor);

	float Previous = 2.f;
	for (float TempC = 0.f; TempC <= Trip + 60.f; TempC += 5.f)
	{
		const float Scale = ThermalDerateScale(TempC, Onset, Trip);
		TestTrue(TEXT("derate never increases with temperature"), Scale <= Previous + 0.001f);
		TestTrue(TEXT("derate never reaches zero - the cutout owns zero torque"), Scale >= ThermalDerateFloor);
		Previous = Scale;
	}

	// Hysteresis: the latch is the reason this one terminal state is stored.
	const float Clear = Spec.CutoutClearTempC;
	TestFalse(TEXT("a cold motor is not cut out"), ThermalCutoutLatch(false, 20.f, Trip, Clear));
	TestTrue(TEXT("reaching the trip cuts out"), ThermalCutoutLatch(false, Trip, Trip, Clear));
	TestTrue(TEXT("between clear and trip a cut-out motor stays cut out"),
		ThermalCutoutLatch(true, 0.5f * (Clear + Trip), Trip, Clear));
	TestFalse(TEXT("between clear and trip a running motor stays running"),
		ThermalCutoutLatch(false, 0.5f * (Clear + Trip), Trip, Clear));
	TestFalse(TEXT("cooling to the clear temperature releases the cutout"),
		ThermalCutoutLatch(true, Clear, Trip, Clear));
	return true;
}

namespace
{
	/**
	 * A transient game world for one test, torn down on scope exit so an early
	 * return cannot leak it. No game mode runs here, so nothing else would
	 * dispatch BeginPlay: NotifyBeginPlay does it and marks the world begun, so
	 * actors spawned afterwards get theirs on spawn like they do in a session.
	 */
	struct FExoneerScopedTestWorld
	{
		UWorld* World = nullptr;

		FExoneerScopedTestWorld()
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

		~FExoneerScopedTestWorld()
		{
			if (!World)
			{
				return;
			}
			// EndPlay first: CleanupWorld warns about a world torn down while
			// it still has begun play, and a warning is a test event.
			World->EndPlay(EEndPlayReason::Quit);
			if (GEngine)
			{
				GEngine->DestroyWorldContext(World);
			}
			World->DestroyWorld(/*bInformEngineOfWorld*/ false);
			World = nullptr;
		}

		FExoneerScopedTestWorld(const FExoneerScopedTestWorld&) = delete;
		FExoneerScopedTestWorld& operator=(const FExoneerScopedTestWorld&) = delete;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerNoWeldHeal,
	"Exoneer.Maintenance.WeldNeverHeals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExoneerNoWeldHeal::RunTest(const FString& Parameters)
{
	// GAME-SCOPE section 10: the weld tool is not a healing beam. This drives
	// the real server routing (UBuildToolComponent::ServerApplyWeld, which
	// Server_Weld forwards to) against a Complete, damaged, half-worn wheel
	// block, so the contract holds on the path the tool actually takes.
	FExoneerScopedTestWorld Scoped;
	UWorld* World = Scoped.World;
	if (!World)
	{
		AddError(TEXT("could not create a transient game world"));
		return false;
	}

	// One wheel block shaped like the bootstrap's: wheel module, tire spare,
	// terrain overlap allowed.
	UVehicleBlockDefinitionDataAsset* Def = NewObject<UVehicleBlockDefinitionDataAsset>(GetTransientPackage());
	Def->BlockId = TEXT("test_wheel");
	Def->SizeInCells = FIntVector(1, 1, 1);
	Def->Mass = 60.f;
	Def->MaxHealth = 250.f;
	Def->ModuleClass = UWheelModule::StaticClass();
	Def->bIsWheel = true;
	Def->bAllowTerrainOverlapOnPlace = true;
	Def->SpareItemId = TEXT("tire");

	EBuildPlacementError PlacementError = EBuildPlacementError::None;
	AVehicleConstruct* Construct = AVehicleConstruct::FoundConstruct(
		World, Def, FTransform(FVector::ZeroVector), PlacementError, 0);
	if (!Construct || Construct->GetBlocks().Num() != 1)
	{
		AddError(FString::Printf(TEXT("FoundConstruct failed, placement error %d"), static_cast<int32>(PlacementError)));
		return false;
	}
	const int32 BlockId = Construct->GetBlocks()[0].BlockInstanceId;

	// Complete, damaged, half worn: exactly what a player would try to weld
	// back to new.
	const float DamagedHealth = 90.f;
	const float WornTreadMm = 5.f;
	TestTrue(TEXT("block restored Complete"),
		Construct->RestoreBlockRecord(BlockId, EConstructionPhase::Complete, 0, 1.f, DamagedHealth, 0.f));
	FPartCondition Worn;
	Worn.TreadDepthMm = WornTreadMm;
	Worn.InflationKPa = Def->WheelSpec.NominalTirePressureKPa;
	TestTrue(TEXT("worn condition restored"), Construct->RestoreBlockCondition(BlockId, Worn));

	// The welder: inside reach, carrying the tool and an EMPTY inventory, so
	// Replace cannot fire and only an illegal heal could move a reading.
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Welder = World->SpawnActor<AActor>(AActor::StaticClass(),
		FTransform(FVector(100.f, 0.f, 0.f)), SpawnParams);
	if (!Welder)
	{
		AddError(TEXT("could not spawn the welder actor"));
		return false;
	}
	UBuildToolComponent* Tool = NewObject<UBuildToolComponent>(Welder);
	Tool->RegisterComponent();
	UInventoryComponent* Inventory = NewObject<UInventoryComponent>(Welder);
	Inventory->RegisterComponent();
	TestTrue(TEXT("welder is inside weld reach"),
		FVector::Dist(Welder->GetActorLocation(), Construct->CellToWorld(FIntVector::ZeroValue)) < Tool->PlacementRange);

	const FVector WeldPoint = Construct->CellToWorld(FIntVector::ZeroValue);
	for (int32 Press = 0; Press < 5; ++Press)
	{
		Tool->ServerApplyWeld(Construct, WeldPoint, 10.f);
	}

	// Feedback code 3 is "already complete": proof the batch reached the
	// re-press branch instead of bailing out at reach or validity, so the
	// assertions below are about the weld and not about a silent no-show.
	TestEqual(TEXT("weld reported already complete"), static_cast<int32>(Tool->LastWeldResult), 3);

	const FVehicleBlockRecord* After = Construct->FindRecord(BlockId);
	if (!After)
	{
		AddError(TEXT("the welded block vanished"));
		return false;
	}
	TestEqual(TEXT("weld restores no health"), After->Health, DamagedHealth);
	TestEqual(TEXT("weld restores no tread"), After->Condition.TreadDepthMm, WornTreadMm);
	TestEqual(TEXT("weld leaves the phase Complete"),
		static_cast<int32>(After->Phase), static_cast<int32>(EConstructionPhase::Complete));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerSpareTireIsNotAMotorRepair,
	"Exoneer.Maintenance.SpareTireIsNotAMotorRepair",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExoneerSpareTireIsNotAMotorRepair::RunTest(const FString& Parameters)
{
	// GAME-SCOPE section 10.4: the failure mode picks the verb, and the spare in
	// the pack is a TIRE. It is legal only at a terminal tread reading, and it
	// restores tread and inflation only - an over-temp cutout recovers by
	// cooling, and motor replace is post-alpha (alpha gate A2).
	FExoneerScopedTestWorld Scoped;
	UWorld* World = Scoped.World;
	if (!World)
	{
		AddError(TEXT("could not create a transient game world"));
		return false;
	}

	UVehicleBlockDefinitionDataAsset* Def = NewObject<UVehicleBlockDefinitionDataAsset>(GetTransientPackage());
	Def->BlockId = TEXT("test_wheel");
	Def->SizeInCells = FIntVector(1, 1, 1);
	Def->Mass = 60.f;
	Def->MaxHealth = 250.f;
	Def->ModuleClass = UWheelModule::StaticClass();
	Def->bIsWheel = true;
	Def->bAllowTerrainOverlapOnPlace = true;
	Def->SpareItemId = TEXT("tire");
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

	// The welder, inside reach, carrying two authored tire spares. Resolved the
	// way ReplacePartAt resolves them, so the test spends the real item.
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Welder = World->SpawnActor<AActor>(AActor::StaticClass(),
		FTransform(FVector(100.f, 0.f, 0.f)), SpawnParams);
	if (!Welder)
	{
		AddError(TEXT("could not spawn the welder actor"));
		return false;
	}
	UBuildToolComponent* Tool = NewObject<UBuildToolComponent>(Welder);
	Tool->RegisterComponent();
	UInventoryComponent* Inventory = NewObject<UInventoryComponent>(Welder);
	Inventory->RegisterComponent();

	const FPrimaryAssetId SpareAsset(TEXT("Item"), Def->SpareItemId);
	UItemDefinitionDataAsset* Spare = Cast<UItemDefinitionDataAsset>(
		UAssetManager::Get().GetPrimaryAssetObject(SpareAsset));
	if (!Spare)
	{
		TSoftObjectPtr<UItemDefinitionDataAsset> Soft(UAssetManager::Get().GetPrimaryAssetPath(SpareAsset));
		Spare = Soft.LoadSynchronous();
	}
	if (!Spare)
	{
		AddError(TEXT("could not resolve the authored tire item (Item:tire)"));
		return false;
	}
	TestEqual(TEXT("two spares fit in the pack"), Inventory->AddItem(Spare, 2), 0);

	const FVector WeldPoint = Construct->CellToWorld(FIntVector::ZeroValue);
	const float CookedWindingC = 150.f;

	// 1. Tread left on the tire: the press is refused and no spare is spent,
	// however hot the motor is. A cutout is not a tire fault.
	FPartCondition Hot;
	Hot.TreadDepthMm = 0.5f * Wheel.NewTreadDepthMm;
	Hot.InflationKPa = Wheel.NominalTirePressureKPa;
	Hot.WindingTempC = CookedWindingC;
	Hot.bThermalCutout = 1;
	TestTrue(TEXT("hot, half worn condition restored"), Construct->RestoreBlockCondition(BlockId, Hot));
	Tool->ServerApplyWeld(Construct, WeldPoint, 10.f);

	const FVehicleBlockRecord* Refused = Construct->FindRecord(BlockId);
	if (!Refused)
	{
		AddError(TEXT("the welded block vanished"));
		return false;
	}
	TestEqual(TEXT("no spare is spent on a tire with tread left"), Inventory->GetItemCount(Spare), 2);
	TestEqual(TEXT("the refused press restores no tread"), Refused->Condition.TreadDepthMm, Hot.TreadDepthMm);
	TestEqual(TEXT("the refused press does not cool the winding"), Refused->Condition.WindingTempC, CookedWindingC);
	TestTrue(TEXT("the refused press leaves the cutout latched"), Refused->Condition.bThermalCutout != 0);

	// 2. Terminal tread: the spare is spent, tread and inflation come back new,
	// and the motor's own readings are untouched.
	FPartCondition Scrap;
	Scrap.TreadDepthMm = 0.f;
	Scrap.InflationKPa = 0.f;
	Scrap.WindingTempC = CookedWindingC;
	Scrap.bThermalCutout = 1;
	TestTrue(TEXT("scrap condition restored"), Construct->RestoreBlockCondition(BlockId, Scrap));
	Tool->ServerApplyWeld(Construct, WeldPoint, 10.f);

	const FVehicleBlockRecord* Replaced = Construct->FindRecord(BlockId);
	if (!Replaced)
	{
		AddError(TEXT("the welded block vanished"));
		return false;
	}
	TestEqual(TEXT("one spare is spent"), Inventory->GetItemCount(Spare), 1);
	TestEqual(TEXT("the new tire is at full tread"), Replaced->Condition.TreadDepthMm, Wheel.NewTreadDepthMm);
	TestEqual(TEXT("the new tire is inflated to nominal"), Replaced->Condition.InflationKPa, Wheel.NominalTirePressureKPa);
	TestEqual(TEXT("a tire does not cool the winding"), Replaced->Condition.WindingTempC, CookedWindingC);
	TestTrue(TEXT("a tire does not clear the thermal cutout"), Replaced->Condition.bThermalCutout != 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerDeckLoadRatio,
	"Exoneer.Maintenance.DeckLoadRatio",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExoneerDeckLoadRatio::RunTest(const FString& Parameters)
{
	using namespace ExoneerMaintenance;

	// The denominator is a WEIGHT, and this pins it: a 5000 kg foundation on a
	// 9.8 m/s^2 planet is rated for 49 kN, so 49 kN reads exactly 1.0. Get the
	// units wrong by a factor of g and every authored capacity is wrong.
	const float GravityMps2 = 9.8f;
	TestTrue(TEXT("5000 kg at 9.8 m/s^2 is 49 kN"),
		FMath::IsNearlyEqual(LoadRatio(49000.f, 5000.f, GravityMps2), 1.f, 0.001f));
	TestTrue(TEXT("half the rated weight is ratio 0.5"),
		FMath::IsNearlyEqual(LoadRatio(24500.f, 5000.f, GravityMps2), 0.5f, 0.001f));

	// An UNRATED piece is judged before any division: it is not a deck, so any
	// wheel at all is a gross overload. Only pieces that reported live load are
	// ever judged, but the arithmetic must not depend on that.
	TestEqual(TEXT("an unrated piece is a gross overload under any wheel"),
		LoadRatio(1.f, 0.f, GravityMps2), GrossOverloadRatio);
	TestEqual(TEXT("a negative rating is still unrated, not a negative ratio"),
		LoadRatio(1.f, -100.f, GravityMps2), GrossOverloadRatio);
	TestEqual(TEXT("a weightless world loads nothing"), LoadRatio(9800.f, 600.f, 0.f), 0.f);

	// The two authored deck cases against the test rover's axle load: about
	// 3.0 t on six wheels is 4.9 kN a wheel, so one axle is 9.8 kN.
	const float AxleLoadN = 9800.f;
	const float LightRatio = LoadRatio(AxleLoadN, 600.f, GravityMps2);
	const float HeavyRatio = LoadRatio(AxleLoadN, 1200.f, GravityMps2);
	TestTrue(TEXT("one axle on road_deck_light is about 1.67"),
		FMath::IsNearlyEqual(LightRatio, 1.667f, 0.01f));
	TestTrue(TEXT("one axle on road_deck is about 0.83"),
		FMath::IsNearlyEqual(HeavyRatio, 0.833f, 0.01f));
	TestTrue(TEXT("the light deck is over its rating"), LightRatio > 1.f);
	TestTrue(TEXT("the heavy deck is inside its rating"), HeavyRatio < 1.f);

	// Permanent set: one way, and 1.5x rating reaches the 60 mm terminal
	// reading in 20 s. The light deck under one axle takes about 15 s.
	TestEqual(TEXT("at the rating a deck takes no set"), DeflectionDeltaMm(1.f, 1.f), 0.f);
	TestEqual(TEXT("below the rating a deck takes no set"), DeflectionDeltaMm(0.83f, 1.f), 0.f);
	TestEqual(TEXT("no time, no set"), DeflectionDeltaMm(1.5f, 0.f), 0.f);

	float DeflectionMm = 0.f;
	for (int32 Step = 0; Step < 200; ++Step)   // 20 s at 0.1 s
	{
		DeflectionMm += DeflectionDeltaMm(1.5f, 0.1f);
	}
	TestTrue(TEXT("1.5x rating reaches 60 mm in 20 s"),
		FMath::IsNearlyEqual(DeflectionMm, 60.f, 0.5f));

	float LightDeflectionMm = 0.f;
	float Seconds = 0.f;
	while (LightDeflectionMm < 60.f && Seconds < 120.f)
	{
		LightDeflectionMm += DeflectionDeltaMm(LightRatio, 0.1f);
		Seconds += 0.1f;
	}
	TestTrue(TEXT("one axle condemns road_deck_light in about 15 s"),
		Seconds > 12.f && Seconds < 18.f);

	// Monotone in load, and never negative.
	float PreviousMm = -1.f;
	for (float Ratio = 0.f; Ratio <= GrossOverloadRatio; Ratio += 0.05f)
	{
		const float DeltaMm = DeflectionDeltaMm(Ratio, 1.f);
		TestTrue(TEXT("set never decreases with load"), DeltaMm >= PreviousMm - KINDA_SMALL_NUMBER);
		TestTrue(TEXT("set is never negative"), DeltaMm >= 0.f);
		PreviousMm = DeltaMm;
	}
	return true;
}

namespace
{
	/** A deck-shaped piece definition: foundation grammar, one free weld stage. */
	UPieceDefinitionDataAsset* MakeDeckDef(FName PieceId, float LoadCapacityKg, bool bGroundable = true)
	{
		UPieceDefinitionDataAsset* Def = NewObject<UPieceDefinitionDataAsset>(GetTransientPackage());
		Def->PieceId = PieceId;
		Def->MountTag = bGroundable ? ExoneerTags::Mount_Foundation : ExoneerTags::Mount_Deployable;
		Def->bGroundable = bGroundable;
		Def->SupportBudget = 8;
		Def->SupportCost = 1;
		Def->MaxHealth = 600.f;
		Def->LoadCapacityKg = LoadCapacityKg;
		Def->TerminalDeflectionMm = 60.f;
		Def->Stages.Add(FConstructionCost());   // no materials, default weld work
		return Def;
	}

	const FName NAME_ExoneerCollapsingTag(TEXT("Exoneer.Collapsing"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerDeckLiveLoad,
	"Exoneer.Maintenance.DeckLiveLoad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExoneerDeckLiveLoad::RunTest(const FString& Parameters)
{
	// V-SPAN on the real actors: the load path from a wheel report through the
	// structure's 5 Hz pass to a permanent set and a collapse.
	FExoneerScopedTestWorld Scoped;
	UWorld* World = Scoped.World;
	if (!World)
	{
		AddError(TEXT("could not create a transient game world"));
		return false;
	}
	// The pass reads g from world settings, which is where the biome writes
	// its own GravityZ. Pin it so the authored capacities read as designed.
	if (AWorldSettings* Settings = World->GetWorldSettings())
	{
		Settings->GlobalGravityZ = -980.f;
		Settings->bGlobalGravitySet = true;
	}
	const float GravityMps2 = FMath::Abs(World->GetGravityZ()) / 100.f;

	UPieceDefinitionDataAsset* LightDef = MakeDeckDef(TEXT("test_road_deck_light"), 600.f);
	EBuildPlacementError Error = EBuildPlacementError::None;
	ABasePiece* Deck = ABaseStructure::PlaceGroundedGhost(World, LightDef, FTransform(FVector::ZeroVector), Error);
	if (!Deck || !Deck->OwningStructure || !Deck->Construction)
	{
		AddError(FString::Printf(TEXT("PlaceGroundedGhost failed, error %d"), static_cast<int32>(Error)));
		return false;
	}
	ABaseStructure* Structure = Deck->OwningStructure;

	// 1. A GHOST carries nothing: reports on an unwelded deck are refused, so
	// a planning marker can never take a permanent set.
	Deck->ReportLiveLoad(4900.f, 1);
	Deck->ReportLiveLoad(4900.f, 1);
	TestEqual(TEXT("a ghost accumulates no load"), Deck->PendingLoadFrames, 0);
	Structure->ServiceLoadReports();
	TestEqual(TEXT("a ghost takes no permanent set"), Deck->Condition.DeflectionMm, 0.f);
	TestEqual(TEXT("a ghost publishes no load reading"), Deck->LastLoadN, 0.f);

	// Weld it. RestoreState is the save path's completion route and fires the
	// same phase delegates a finished weld does.
	Deck->Construction->RestoreState(EConstructionPhase::Complete, 0, 1.f);
	TestTrue(TEXT("the deck is Complete"), Deck->Construction->IsComplete());
	TestEqual(TEXT("a grounded deck seeds its own support budget"),
		Deck->SupportValue, LightDef->SupportBudget);

	// 2. The mean-of-frames read. Two wheels of one axle report 4.9 kN each
	// inside every frame, over three frames. The piece must read the AXLE
	// (9.8 kN) and not the per-wheel mean, or a deck rated in kilograms is
	// measured against the wrong number of wheels.
	for (uint64 Frame = 1; Frame <= 3; ++Frame)
	{
		Deck->ReportLiveLoad(4900.f, Frame);
		Deck->ReportLiveLoad(4900.f, Frame);
	}
	TestEqual(TEXT("three frames of reports count as three frames"), Deck->PendingLoadFrames, 3);
	TestTrue(TEXT("the accumulated sum is six wheel reports"),
		FMath::IsNearlyEqual(Deck->PendingLoadN, 6.f * 4900.f, 1.f));

	Structure->ServiceLoadReports();
	TestTrue(TEXT("the published reading is one axle, not one wheel"),
		FMath::IsNearlyEqual(Deck->LastLoadN, 9800.f, 1.f));
	TestEqual(TEXT("the accumulators are drained"), Deck->PendingLoadFrames, 0);

	// One 0.2 s pass at ratio 1.667 is 0.8 mm of set, past the 0.5 mm deadband
	// so it publishes at once.
	const float ExpectedMm = ExoneerMaintenance::DeflectionDeltaMm(
		ExoneerMaintenance::LoadRatio(9800.f, 600.f, GravityMps2), 0.2f);
	TestTrue(TEXT("one pass over the rating spends permanent set"),
		FMath::IsNearlyEqual(Deck->Condition.DeflectionMm, ExpectedMm, 0.01f));
	TestFalse(TEXT("one pass does not condemn the deck"), Deck->IsLoadCondemned());
	TestFalse(TEXT("a sagging deck is not collapsing yet"),
		Deck->Tags.Contains(NAME_ExoneerCollapsingTag));

	// 3. Keep the axle on it: about 15 s of passes reach the terminal reading.
	for (int32 Pass = 0; Pass < 200 && !Deck->IsLoadCondemned(); ++Pass)
	{
		const uint64 Frame = 100 + static_cast<uint64>(Pass);
		Deck->ReportLiveLoad(4900.f, Frame);
		Deck->ReportLiveLoad(4900.f, Frame);
		Structure->ServiceLoadReports();
	}
	TestTrue(TEXT("the axle condemns the light deck"), Deck->IsLoadCondemned());
	TestTrue(TEXT("permanent set never came back down"),
		Deck->Condition.DeflectionMm >= LightDef->TerminalDeflectionMm);

	// A condemned deck carries nothing: the next wheel on it is a gross
	// overload, which drops its support and hands it to the existing collapse
	// batch (tagged here; the 0.5 s timer owns the destruction).
	Deck->ReportLiveLoad(4900.f, 900);
	Structure->ServiceLoadReports();
	TestTrue(TEXT("a condemned deck under a wheel is a gross overload"), Deck->bLoadOverloaded);
	TestEqual(TEXT("an overloaded piece keeps no support"), Deck->SupportValue, 0);
	TestTrue(TEXT("the overloaded deck is batched for collapse"),
		Deck->Tags.Contains(NAME_ExoneerCollapsingTag));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerGroundedDeployableStands,
	"Exoneer.Maintenance.GroundedDeployableStands",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExoneerGroundedDeployableStands::RunTest(const FString& Parameters)
{
	// A deployable welded straight onto terrain founds its own structure and
	// has no socket parent. It used to read as an orphan and get collapsed
	// 0.5 s later - the bug the dish would have hit. The solver now seeds it
	// with the same predicate the build tool uses to decide what may be placed
	// on terrain at all.
	FExoneerScopedTestWorld Scoped;
	UWorld* World = Scoped.World;
	if (!World)
	{
		AddError(TEXT("could not create a transient game world"));
		return false;
	}

	UPieceDefinitionDataAsset* Def = MakeDeckDef(TEXT("test_dish"), 400.f, /*bGroundable*/ false);
	Def->SupportBudget = 1;
	TestFalse(TEXT("a deployable is not groundable"), Def->bGroundable);
	TestTrue(TEXT("a deployable carries the deployable mount tag"),
		Def->MountTag == ExoneerTags::Mount_Deployable);

	EBuildPlacementError Error = EBuildPlacementError::None;
	ABasePiece* Piece = ABaseStructure::PlaceGroundedGhost(World, Def, FTransform(FVector::ZeroVector), Error);
	if (!Piece || !Piece->OwningStructure || !Piece->Construction)
	{
		AddError(FString::Printf(TEXT("PlaceGroundedGhost failed, error %d"), static_cast<int32>(Error)));
		return false;
	}

	// As a ghost: a planning marker on terrain must survive to be welded.
	Piece->OwningStructure->RecomputeSupport();
	TestFalse(TEXT("a ghost deployable on terrain is not batched for collapse"),
		Piece->Tags.Contains(NAME_ExoneerCollapsingTag));

	// Complete: it seeds its own support budget and stays exempt.
	Piece->Construction->RestoreState(EConstructionPhase::Complete, 0, 1.f);
	TestEqual(TEXT("a grounded deployable seeds its own budget"), Piece->SupportValue, Def->SupportBudget);
	TestFalse(TEXT("a completed deployable on terrain is not batched for collapse"),
		Piece->Tags.Contains(NAME_ExoneerCollapsingTag));
	TestTrue(TEXT("the piece is still alive"), IsValid(Piece));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerBatteryCapacityFade,
	"Exoneer.Maintenance.BatteryCapacityFade",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExoneerBatteryCapacityFade::RunTest(const FString& Parameters)
{
	using namespace ExoneerMaintenance;

	// The authored vehicle battery block: 900 kJ rated.
	const float RatedJ = 900000.f;

	// Monotone in throughput, and nothing for nothing.
	TestEqual(TEXT("no throughput, no fade"), CapacityFadeDelta(0.f, RatedJ, 20.f), 0.f);
	TestEqual(TEXT("an unrated pack never fades"), CapacityFadeDelta(RatedJ, 0.f, 20.f), 0.f);
	float Previous = -1.f;
	for (float Fraction = 0.f; Fraction <= 2.f; Fraction += 0.1f)
	{
		const float Delta = CapacityFadeDelta(Fraction * RatedJ, RatedJ, 20.f);
		TestTrue(TEXT("fade never decreases with throughput"), Delta >= Previous - KINDA_SMALL_NUMBER);
		TestTrue(TEXT("fade is never negative"), Delta >= 0.f);
		Previous = Delta;
	}

	// Monotone in temperature: flat up to the onset, then linear.
	TestEqual(TEXT("cold cells age at the same rate as cells at the onset"),
		CapacityFadeDelta(RatedJ, RatedJ, 10.f),
		CapacityFadeDelta(RatedJ, RatedJ, CapacityFadeTempOnsetC));
	TestTrue(TEXT("hot cells fade faster"),
		CapacityFadeDelta(RatedJ, RatedJ, 60.f) > CapacityFadeDelta(RatedJ, RatedJ, 20.f));
	TestTrue(TEXT("20 C over the onset doubles the rate"),
		FMath::IsNearlyEqual(CapacityFadeDelta(RatedJ, RatedJ, CapacityFadeTempOnsetC + 20.f),
			2.f * CapacityFadeDelta(RatedJ, RatedJ, 20.f), KINDA_SMALL_NUMBER));
	Previous = -1.f;
	for (float TempC = -40.f; TempC <= 120.f; TempC += 5.f)
	{
		const float Delta = CapacityFadeDelta(RatedJ, RatedJ, TempC);
		TestTrue(TEXT("fade never decreases with temperature"), Delta >= Previous - KINDA_SMALL_NUMBER);
		Previous = Delta;
	}

	// One full charge plus the discharge after it is 2 * RatedJ of
	// throughput. At nominal temperature the floor is 120 of them.
	const float CycleJ = 2.f * RatedJ;
	float Fade = 0.f;
	int32 Cycles = 0;
	while (!IsCapacityTerminal(Fade) && Cycles < 10000)
	{
		Fade = ApplyCapacityFade(Fade, CapacityFadeDelta(CycleJ, RatedJ, 20.f));
		++Cycles;
	}
	TestEqual(TEXT("the floor is 120 full cycles at nominal temperature"), Cycles, 120);

	// A hot pack gets there sooner - same joules, fewer cycles.
	float HotFade = 0.f;
	int32 HotCycles = 0;
	while (!IsCapacityTerminal(HotFade) && HotCycles < 10000)
	{
		HotFade = ApplyCapacityFade(HotFade, CapacityFadeDelta(CycleJ, RatedJ, 55.f));
		++HotCycles;
	}
	TestEqual(TEXT("at 55 C the same pack is spent in half the cycles"), HotCycles, 60);

	// The floor holds however hard the pack is thrashed, and 40 percent of the
	// rating is still there when it does.
	Fade = ApplyCapacityFade(Fade, CapacityFadeDelta(1000.f * CycleJ, RatedJ, 90.f));
	TestEqual(TEXT("fade never passes the floor"), Fade, CapacityFadeFloor);
	TestTrue(TEXT("a spent pack still holds 40 percent of its rating"),
		FMath::IsNearlyEqual(EffectiveCapacityJ(RatedJ, Fade), 0.4f * RatedJ, 1.f));
	TestTrue(TEXT("a spent pack reads terminal"), IsCapacityTerminal(Fade));
	TestFalse(TEXT("a fresh pack does not read terminal"), IsCapacityTerminal(0.f));
	TestFalse(TEXT("a half worn pack does not read terminal"), IsCapacityTerminal(0.3f));
	TestEqual(TEXT("a fresh pack holds its rating"), EffectiveCapacityJ(RatedJ, 0.f), RatedJ);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerBatteryCellReplace,
	"Exoneer.Maintenance.BatteryCellReplace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExoneerBatteryCellReplace::RunTest(const FString& Parameters)
{
	// The piece Replace verb on the path the build tool actually takes: a
	// re-press on a Complete battery bank. Legal only at the terminal
	// reading, spends one fabricated cell, and heals nothing.
	FExoneerScopedTestWorld Scoped;
	UWorld* World = Scoped.World;
	if (!World)
	{
		AddError(TEXT("could not create a transient game world"));
		return false;
	}

	const float RatedJ = 600000.f;   // the authored battery bank piece
	UPieceDefinitionDataAsset* Def = MakeDeckDef(TEXT("test_battery"), 600.f, /*bGroundable*/ false);
	Def->PieceClass = ABatteryPiece::StaticClass();
	Def->EnergyStorage = RatedJ;
	Def->SpareItemId = TEXT("battery_cell");

	EBuildPlacementError PlacementError = EBuildPlacementError::None;
	ABasePiece* Piece = ABaseStructure::PlaceGroundedGhost(World, Def, FTransform(FVector::ZeroVector), PlacementError);
	AMachinePiece* Machine = Cast<AMachinePiece>(Piece);
	if (!Machine || !Machine->Construction || !Machine->Power)
	{
		AddError(FString::Printf(TEXT("PlaceGroundedGhost failed, error %d"), static_cast<int32>(PlacementError)));
		return false;
	}
	Machine->Construction->RestoreState(EConstructionPhase::Complete, 0, 1.f);

	// The welder, inside reach, carrying two authored cells.
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Welder = World->SpawnActor<AActor>(AActor::StaticClass(),
		FTransform(FVector(100.f, 0.f, 0.f)), SpawnParams);
	if (!Welder)
	{
		AddError(TEXT("could not spawn the welder actor"));
		return false;
	}
	UBuildToolComponent* Tool = NewObject<UBuildToolComponent>(Welder);
	Tool->RegisterComponent();
	UInventoryComponent* Inventory = NewObject<UInventoryComponent>(Welder);
	Inventory->RegisterComponent();

	const FPrimaryAssetId SpareAsset(TEXT("Item"), Def->SpareItemId);
	UItemDefinitionDataAsset* Spare = Cast<UItemDefinitionDataAsset>(
		UAssetManager::Get().GetPrimaryAssetObject(SpareAsset));
	if (!Spare)
	{
		TSoftObjectPtr<UItemDefinitionDataAsset> Soft(UAssetManager::Get().GetPrimaryAssetPath(SpareAsset));
		Spare = Soft.LoadSynchronous();
	}
	if (!Spare)
	{
		AddError(TEXT("could not resolve the authored spare cell (Item:battery_cell)"));
		return false;
	}
	TestEqual(TEXT("two cells fit in the pack"), Inventory->AddItem(Spare, 2), 0);

	const FVector WeldPoint = Machine->GetActorLocation();
	const float DamagedHealth = 120.f;

	// 1. Cycles left in the pack: the press is refused, no cell is spent, and
	// the derated capacity is exactly what the fade says.
	Machine->Condition.CapacityFade01 = 0.3f;
	Machine->Health = DamagedHealth;
	Machine->ApplyDefinitionStats();
	Machine->Power->StoredEnergy = 100000.f;
	TestTrue(TEXT("a 30 percent faded pack holds 70 percent of its rating"),
		FMath::IsNearlyEqual(Machine->Power->StorageCapacity, 0.7f * RatedJ, 1.f));
	TestFalse(TEXT("a pack with cycles left is not terminal"), Machine->IsConditionTerminal());

	Tool->ServerApplyWeld(Machine, WeldPoint, 10.f);
	TestEqual(TEXT("weld reported already complete"), static_cast<int32>(Tool->LastWeldResult), 3);
	TestEqual(TEXT("no cell is spent on a pack with cycles left"), Inventory->GetItemCount(Spare), 2);
	TestEqual(TEXT("the refused press restores no capacity"), Machine->Condition.CapacityFade01, 0.3f);
	TestEqual(TEXT("the refused press heals nothing"), Machine->Health, DamagedHealth);

	// 2. At the fade floor the cell is legal: it is spent, the pack comes back
	// at its rating and EMPTY, and the piece's health is still where it was.
	Machine->Condition.CapacityFade01 = ExoneerMaintenance::CapacityFadeFloor;
	Machine->ApplyDefinitionStats();
	Machine->Power->StoredEnergy = 100000.f;
	TestTrue(TEXT("a spent pack reads terminal"), Machine->IsConditionTerminal());

	Tool->ServerApplyWeld(Machine, WeldPoint, 10.f);
	TestEqual(TEXT("the replace press reported a swap"), static_cast<int32>(Tool->LastWeldResult), 6);
	TestEqual(TEXT("one cell is spent"), Inventory->GetItemCount(Spare), 1);
	TestEqual(TEXT("the new cells read nominal"), Machine->Condition.CapacityFade01, 0.f);
	TestTrue(TEXT("the pack is back at its rating"),
		FMath::IsNearlyEqual(Machine->Power->StorageCapacity, RatedJ, 1.f));
	TestEqual(TEXT("the joules in the old cells left with them"), Machine->Power->StoredEnergy, 0.f);
	TestEqual(TEXT("a cell swap heals nothing"), Machine->Health, DamagedHealth);
	TestTrue(TEXT("the piece is still standing"), IsValid(Machine));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerSealLeakGrowthMonotone,
	"Exoneer.Maintenance.SealLeakGrowthMonotone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExoneerSealLeakGrowthMonotone::RunTest(const FString& Parameters)
{
	using namespace ExoneerMaintenance;

	TestEqual(TEXT("no storm, no leak"), SealLeakGrowthLps(0.f, 0), 0.f);
	TestEqual(TEXT("no storm, a worn seal still takes nothing from weather"), SealLeakGrowthLps(0.f, 3), 0.f);

	float Previous = -1.f;
	for (uint8 Patches = 0; Patches <= SealPatchCap; ++Patches)
	{
		const float Rate = SealLeakGrowthLps(1.f, Patches);
		TestTrue(TEXT("storm leak never decreases with patch count"), Rate >= Previous - KINDA_SMALL_NUMBER);
		TestTrue(TEXT("a full storm always spends a leaking seal"), Rate > 0.f);
		Previous = Rate;
	}

	float Fresh = 0.f;
	for (int32 Second = 0; Second < 250; ++Second)
	{
		Fresh += SealLeakGrowthLps(1.f, 0);
	}
	TestTrue(TEXT("a fresh seal reaches the emergency line after about 250 s of full exposure"),
		FMath::IsNearlyEqual(Fresh, SealEmergencyLps, 0.001f));

	float Worn = 0.f;
	for (int32 Second = 0; Second < 100; ++Second)
	{
		Worn += SealLeakGrowthLps(1.f, SealPatchCap);
	}
	TestTrue(TEXT("a thrice-patched seal reaches the emergency line after about 100 s of full exposure"),
		FMath::IsNearlyEqual(Worn, SealEmergencyLps, 0.001f));

	TestEqual(TEXT("a landing at the threshold spends nothing"), SealLandingLeakDeltaLps(SealLandingSpeedMps), 0.f);
	TestTrue(TEXT("a harder landing spends more leak"),
		SealLandingLeakDeltaLps(12.f) > SealLandingLeakDeltaLps(10.f));
	TestEqual(TEXT("a 10 m/s landing adds 0.008 L/s"), SealLandingLeakDeltaLps(10.f), 0.008f);

	TestEqual(TEXT("the first patch leaves a 0.005 L/s seep"), SealLeakAfterPatch(0), SealPatchBaseLps);
	TestEqual(TEXT("the third patch leaves a 0.015 L/s seep"), SealLeakAfterPatch(2), SealPatchBaseLps * 3.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerSealPatchCapRefused,
	"Exoneer.Maintenance.SealPatchCapRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FExoneerSealPatchCapRefused::RunTest(const FString& Parameters)
{
	FExoneerScopedTestWorld Scoped;
	UWorld* World = Scoped.World;
	if (!World)
	{
		AddError(TEXT("could not create a transient game world"));
		return false;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Owner = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParams);
	if (!Owner)
	{
		AddError(TEXT("could not spawn the suit owner"));
		return false;
	}

	USurvivalStatsComponent* Stats = NewObject<USurvivalStatsComponent>(Owner);
	Stats->RegisterComponent();
	UInventoryComponent* Inventory = NewObject<UInventoryComponent>(Owner);
	Inventory->RegisterComponent();

	Stats->SuitCondition.LeakRateLps = 0.2f;
	Stats->SuitCondition.PatchCount = ExoneerMaintenance::SealPatchCap;
	TestFalse(TEXT("a kit is refused at the patch cap"), Stats->TryPatchSeal(Inventory));
	TestEqual(TEXT("the refused press leaves the patch count"),
		static_cast<int32>(Stats->SuitCondition.PatchCount), static_cast<int32>(ExoneerMaintenance::SealPatchCap));
	TestTrue(TEXT("the refused press leaves the leak"),
		FMath::IsNearlyEqual(Stats->SuitCondition.LeakRateLps, 0.2f));

	TestFalse(TEXT("replace without a spare is refused"), Stats->TryReplaceSeal(Inventory));
	TestEqual(TEXT("a refused replace leaves the worn seal"),
		static_cast<int32>(Stats->SuitCondition.PatchCount), static_cast<int32>(ExoneerMaintenance::SealPatchCap));
	return true;
}

#endif
