// Copyright Exoneer contributors.
#include "World/TestRoverSpawner.h"
#include "Exoneer.h"
#include "Vehicles/VehicleConstruct.h"
#include "Vehicles/PilotInput.h"
#include "Vehicles/VehicleModule.h"
#include "Vehicles/VehicleOrientation.h"
#include "Data/VehicleBlockDefinitionDataAsset.h"
#include "Player/PlayerSurvivalCharacter.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"

namespace
{
	UVehicleBlockDefinitionDataAsset* LoadBlockDef(const TCHAR* AssetName)
	{
		const FString Path = FString::Printf(TEXT("/Game/Exoneer/Data/VehicleBlocks/%s.%s"), AssetName, AssetName);
		return LoadObject<UVehicleBlockDefinitionDataAsset>(nullptr, *Path);
	}
}

ATestRoverSpawner::ATestRoverSpawner()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void ATestRoverSpawner::BeginPlay()
{
	Super::BeginPlay();
	if (HasAuthority())
	{
		SpawnRover();
	}
}

void ATestRoverSpawner::SpawnRover()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	// A rover from an earlier session or a loaded save takes precedence.
	for (TActorIterator<AVehicleConstruct> It(World); It; ++It)
	{
		if (FVector::Dist(It->GetActorLocation(), GetActorLocation()) < ExistingConstructCheckRadius)
		{
			SpawnedRover = *It;
			return;
		}
	}

	UVehicleBlockDefinitionDataAsset* FrameDef = LoadBlockDef(TEXT("DA_Block_Frame"));
	UVehicleBlockDefinitionDataAsset* CockpitDef = LoadBlockDef(TEXT("DA_Block_Cockpit"));
	UVehicleBlockDefinitionDataAsset* BatteryDef = LoadBlockDef(TEXT("DA_Block_Battery"));
	UVehicleBlockDefinitionDataAsset* SolarDef = LoadBlockDef(TEXT("DA_Block_Solar"));
	UVehicleBlockDefinitionDataAsset* WheelSteerDef = LoadBlockDef(TEXT("DA_Block_WheelRoadSteer"));
	UVehicleBlockDefinitionDataAsset* WheelDriveDef = LoadBlockDef(TEXT("DA_Block_WheelRoadDrive"));
	UVehicleBlockDefinitionDataAsset* ThrusterDef = LoadBlockDef(TEXT("DA_Block_Thruster"));
	UVehicleBlockDefinitionDataAsset* GyroDef = LoadBlockDef(TEXT("DA_Block_Gyro"));
	if (!FrameDef || !CockpitDef || !BatteryDef || !SolarDef || !WheelSteerDef || !WheelDriveDef
		|| !ThrusterDef || !GyroDef)
	{
		UE_LOG(LogExoneer, Warning, TEXT("TestRoverSpawner: vehicle block definitions missing (run the bootstrap); no rover spawned."));
		return;
	}

	struct FPlacement
	{
		UVehicleBlockDefinitionDataAsset* Def;
		FIntVector Origin;
		float StateScalar;
		uint8 Orientation = 0;
	};
	TArray<FPlacement> Placements;

	// EVERY thruster aim index comes from the SAME two-axis helper the build
	// tool's aim list uses, so a spawned and a hand-built rover can never
	// disagree about which orientation means "thrust up, toed left".
	//
	// THE ONE-AXIS LOOKUP IS NOT USED HERE ANY MORE, and that is the fix for a
	// defect that made the shipped rover unflyable a second time. Every
	// thruster carries the nozzle cant (DA_Block_Thruster NozzleCantDeg leans
	// the jet toward the block's own local +Y), so four orientations aim a unit
	// the same way and differ only in WHICH WAY THE JET LEANS.
	// FindOrientationMappingAxis returns the first of the four, which for
	// "forward" is index 2 - and index 2 leans BOTH forward units the same way.
	// Two 418 N side thrusts that do not cancel, both 1.277 m behind the centre
	// of mass, are a 1022 N*m standing YAW moment at full throttle and 753 N of
	// side force no control can answer, because flight has no lateral thrust
	// command at all. Measured on the shipped craft: 920 N*m of standing yaw at
	// the 0.90 ceiling against about 400 N*m of yaw trim authority, so 469 N*m
	// went into the rotors at hover and 624 N*m at the ceiling - the yaw axis
	// filled 3.3 s after the pilot first held W and the hull then spun up to
	// 114 deg/s with the stick centred. Mounted as a MIRRORED PAIR below, the
	// two lateral components and the two x_off * F_y yaw moments cancel exactly,
	// standing yaw falls to -41 N*m (fully trimmed, residual 0 N*m on all three
	// axes) and side force to 0.0 N.
	const uint8 ThrustUpToeLeft = ExoneerVehicleOrientation::FindOrientationMappingAxes(
		ExoneerThruster::LocalThrustAxis, FVector::UpVector, FVector::YAxisVector, -FVector::YAxisVector);
	const uint8 ThrustUpToeRight = ExoneerVehicleOrientation::FindOrientationMappingAxes(
		ExoneerThruster::LocalThrustAxis, FVector::UpVector, FVector::YAxisVector, FVector::YAxisVector);
	const uint8 ThrustForwardToeLeft = ExoneerVehicleOrientation::FindOrientationMappingAxes(
		ExoneerThruster::LocalThrustAxis, FVector::ForwardVector, FVector::YAxisVector, -FVector::YAxisVector);
	const uint8 ThrustForwardToeRight = ExoneerVehicleOrientation::FindOrientationMappingAxes(
		ExoneerThruster::LocalThrustAxis, FVector::ForwardVector, FVector::YAxisVector, FVector::YAxisVector);

	// --- Ladder chassis, 12 x 4 cells (3.00 m x 1.00 m), Z = 0 ---
	// A ladder, not a slab: 31 frames instead of 60 keeps the rover near 3 t
	// instead of 4.5 t. Order matters - the spawner places in one pass, so
	// every block must already touch something placed.
	// Left longeron (Y = 0). The founder occupies (0,0,0).
	for (int32 X = 1; X <= 11; ++X)
	{
		Placements.Add({ FrameDef, FIntVector(X, 0, 0), 0.f });
	}
	// Rear crossmember first: it is the only bridge across to the right rail.
	Placements.Add({ FrameDef, FIntVector(1, 1, 0), 0.f });
	Placements.Add({ FrameDef, FIntVector(1, 2, 0), 0.f });
	// Right longeron (Y = 3), starting at the cell that touches the bridge.
	for (int32 X = 1; X <= 11; ++X)
	{
		Placements.Add({ FrameDef, FIntVector(X, 3, 0), 0.f });
	}
	Placements.Add({ FrameDef, FIntVector(0, 3, 0), 0.f });
	// Remaining crossmembers: mid axle, front axle, nose.
	for (const int32 X : { 5, 9, 11 })
	{
		Placements.Add({ FrameDef, FIntVector(X, 1, 0), 0.f });
		Placements.Add({ FrameDef, FIntVector(X, 2, 0), 0.f });
	}

	// --- Wheels: three axles at 1.00 m spacing, track 1.25 m ---
	// Each is 3x1x3 cells with Origin.Z = -1, so it spans Z -1..1 and its
	// inboard column touches the rail row at Z = 0. Axle is block local Y, so
	// orientation 0 is correct.
	// Only the FRONT pair steers: ServerRouteDrive derives the Ackermann turn
	// centre from the centroid of the NON-steered wheels, which is the one
	// split that solver resolves correctly (a steered rear pair would crab).
	// All six are driven (the steer spec keeps bDriven), so this is 6x6.
	Placements.Add({ WheelDriveDef, FIntVector(0, -1, -1), 0.f });
	Placements.Add({ WheelDriveDef, FIntVector(0,  4, -1), 0.f });
	Placements.Add({ WheelDriveDef, FIntVector(4, -1, -1), 0.f });
	Placements.Add({ WheelDriveDef, FIntVector(4,  4, -1), 0.f });
	Placements.Add({ WheelSteerDef, FIntVector(8, -1, -1), 0.f });
	Placements.Add({ WheelSteerDef, FIntVector(8,  4, -1), 0.f });

	// --- Module bay down the centre lane (Y = 1..2), Z = 1 ---
	// Battery bank over the rear axle, delivered full (StateScalar 1).
	Placements.Add({ BatteryDef, FIntVector(1, 1, 1), 1.f });
	Placements.Add({ BatteryDef, FIntVector(1, 2, 1), 1.f });
	Placements.Add({ BatteryDef, FIntVector(2, 1, 1), 1.f });
	Placements.Add({ BatteryDef, FIntVector(2, 2, 1), 1.f });
	// Attitude gyro (2x2x2: X 5..6, Y 1..2, Z 1..2) on the mid crossmember.
	// Must precede the solar pair, which anchors to its cells.
	Placements.Add({ GyroDef, FIntVector(5, 1, 1), 0.f });
	// Second gyro (X 7..8): one unit is marginal in yaw on a ~2 t hull, two
	// give the pilot real authority and let attitude hold actually settle it.
	Placements.Add({ GyroDef, FIntVector(7, 1, 1), 0.f });
	Placements.Add({ SolarDef, FIntVector(4, 1, 1), 0.f });
	Placements.Add({ SolarDef, FIntVector(4, 2, 1), 0.f });
	Placements.Add({ CockpitDef, FIntVector(11, 1, 1), 0.f });

	// --- Thrusters ---
	// Six lifting, on the outer rails at X = 2 / 5 / 8, TOED OUTBOARD: the
	// Y = 0 rail leans its jets toward -Y and the Y = 3 rail toward +Y. Their
	// centroid lands within 3 cm of the centre of mass, so full lift is very
	// nearly pure force and the trim path cancels the rest.
	//
	// THE TOE IS WHY THIS CRAFT HAS YAW AUTHORITY IN THE AIR AT ALL. Six
	// thrusters pointing along the hull's own up axis make exactly zero yaw
	// moment however they are throttled - so before this, the only yaw
	// effector was the forward pair, whose trim authority is zero unless the
	// pilot happens to be holding W. Holding 30 deg/s of yaw costs
	// 0.15 * 1544 * 0.52 = 120 N*m*s of rotor momentum EVERY SECOND, so the
	// axis saturated after 202 degrees of one continuous turn and the mouse
	// went dead in that direction until the pilot yawed back or landed. Toed
	// out 6 degrees, a diagonal trim across the four corner units makes about
	// 300 N*m of pure yaw couple with no net force, so thrust carries the hold
	// cost and the rotors only pay for the transient.
	for (const int32 X : { 2, 5, 8 })
	{
		Placements.Add({ ThrusterDef, FIntVector(X, 0, 1), 0.f, ThrustUpToeLeft });
		Placements.Add({ ThrusterDef, FIntVector(X, 3, 1), 0.f, ThrustUpToeRight });
	}
	// Two facing forward, STRADDLING the centre of mass in Z: one in the open
	// rear crossmember cell at Z = 0, 0.159 m below the CoM, and one at Z = 1,
	// 0.091 m above it. Their pitch moments largely cancel, so holding the
	// forward key adds 270 N*m of standing pitch rather than the 1224 N*m a
	// pair mounted entirely below the CoM makes. Mass, thrust and power draw
	// are unchanged - this is the same two thrusters, mounted where the
	// arithmetic works.
	//
	// (The craft's TOTAL standing pitch is larger than either figure, because
	// the lift group's centroid sits 27 mm aft of the CoM: 485 N*m at the hover
	// collective with no key held, 241 N*m with W held - the forward pair's
	// nose-up residual partly cancels it - and 334 N*m at the ceiling. All of
	// it is cancelled by the trim, which is why the number that matters is the
	// RESIDUAL and the residual is zero.)
	//
	// THIS IS THE DEFECT THAT MADE THE SHIPPED ROVER UNFLYABLE. A standing
	// moment never ends, and a reaction wheel can only hold one by winding its
	// rotors: 1224 N*m against the two triads' 1600 N*m*s per axis filled the
	// pitch axis in 1.3 s of holding W, after which nothing could stop the
	// craft pitching up. The differential-thrust trim in ServerRouteThrust now
	// cancels whatever standing moment a build makes, for free, whenever the
	// lift valve is off its stops - but the SHIPPED vehicle must not need
	// rescuing, so the geometry is fixed here too. 270 N*m sits far inside the
	// trim authority at every collective the pilot flies, and even with the
	// valve shut the axis takes about 6 s rather than 1.3 s to reach the stop.
	//
	// The residual sign is deliberately NOSE-UP: pitching up tilts the lift
	// vector back and bleeds forward speed, so the coupling self-limits. The
	// opposite residual pitches the nose down, which adds forward thrust, which
	// pitches it further down - that one diverges.
	//
	// These two are NOT the yaw effector, and treating them as one was the
	// architectural error: a forward-facing unit's trim bound is bounded by
	// its own base throttle, so its yaw authority is exactly zero whenever the
	// pilot is not asking to go forward. The lift toe above is the yaw
	// effector, and it works with no key held.
	//
	// AND THEY ARE A MIRRORED PAIR: the unit on the -Y side of the centre of
	// mass toes left, the one on the +Y side toes right. The cant is on the
	// block DEFINITION, so a forward-facing unit gets it whether it wants it or
	// not; the only question a placement can answer is which way it leans, and
	// leaning both the same way is what put 920 N*m of uncancellable yaw on
	// this craft. Mirrored, the two 418 N lateral components cancel and so do
	// the two x_off * F_y yaw moments, exactly, the way the lift rails already
	// do - and nothing else about the craft changes.
	Placements.Add({ ThrusterDef, FIntVector(0, 1, 0), 0.f, ThrustForwardToeLeft });
	Placements.Add({ ThrusterDef, FIntVector(0, 2, 1), 0.f, ThrustForwardToeRight });

	// Centre the rover on the garage pad. The actor pivot is the chassis
	// rear-left-bottom corner, so a rover founded at the spawner would grow
	// 3 m in +X and sit visibly off-centre. Derived from the cells actually
	// placed, so editing the layout above cannot silently push it off the pad.
	FIntVector MinCell = FIntVector::ZeroValue;
	FIntVector MaxCell = FIntVector::ZeroValue;
	TArray<FIntVector> Cells;
	for (const FPlacement& Placement : Placements)
	{
		ExoneerVehicleOrientation::GetOccupiedCells(Placement.Origin, Placement.Def->SizeInCells, Placement.Orientation, Cells);
		for (const FIntVector& Cell : Cells)
		{
			MinCell = FIntVector(FMath::Min(MinCell.X, Cell.X), FMath::Min(MinCell.Y, Cell.Y), FMath::Min(MinCell.Z, Cell.Z));
			MaxCell = FIntVector(FMath::Max(MaxCell.X, Cell.X), FMath::Max(MaxCell.Y, Cell.Y), FMath::Max(MaxCell.Z, Cell.Z));
		}
	}
	const FVector LocalCentre(
		(MinCell.X + MaxCell.X + 1) * 0.5f * AVehicleConstruct::CellSize,
		(MinCell.Y + MaxCell.Y + 1) * 0.5f * AVehicleConstruct::CellSize,
		0.f);

	FTransform SpawnTransform = GetActorTransform();
	SpawnTransform.SetScale3D(FVector::OneVector);
	SpawnTransform.SetLocation(GetActorLocation()
		- GetActorQuat().RotateVector(LocalCentre)
		+ FVector(0.f, 0.f, SpawnHeightUU));

	EBuildPlacementError Error = EBuildPlacementError::None;
	AVehicleConstruct* Rover = AVehicleConstruct::FoundConstruct(World, FrameDef, SpawnTransform, Error);
	if (!Rover || Rover->GetBlockCount() == 0)
	{
		UE_LOG(LogExoneer, Warning, TEXT("TestRoverSpawner: FoundConstruct failed (error %d)."), (int32)Error);
		return;
	}

	TArray<int32> PlacedIds;
	PlacedIds.Add(Rover->GetBlocks()[0].BlockInstanceId);
	TArray<const UVehicleBlockDefinitionDataAsset*> PlacedDefs;
	PlacedDefs.Add(FrameDef);
	TArray<float> PlacedScalars;
	PlacedScalars.Add(0.f);

	for (const FPlacement& Placement : Placements)
	{
		const int32 BlockId = Rover->PlaceBlockGhost(Placement.Def, Placement.Origin, Placement.Orientation);
		if (BlockId == INDEX_NONE)
		{
			// An Error, not a Warning: with 50+ placements one bad cell ships a
			// silently crippled rover.
			UE_LOG(LogExoneer, Error, TEXT("TestRoverSpawner: could not place %s at (%d,%d,%d)."),
				*Placement.Def->BlockId.ToString(), Placement.Origin.X, Placement.Origin.Y, Placement.Origin.Z);
			continue;
		}
		PlacedIds.Add(BlockId);
		PlacedDefs.Add(Placement.Def);
		PlacedScalars.Add(Placement.StateScalar);
	}
	if (PlacedIds.Num() != Placements.Num() + 1)
	{
		UE_LOG(LogExoneer, Error, TEXT("TestRoverSpawner: %d of %d blocks failed to place - the layout is wrong."),
			Placements.Num() + 1 - PlacedIds.Num(), Placements.Num() + 1);
	}

	// Weld everything to Complete in one pass - this is a delivered vehicle,
	// not a construction exercise.
	for (int32 Index = 0; Index < PlacedIds.Num(); ++Index)
	{
		const UVehicleBlockDefinitionDataAsset* Def = PlacedDefs[Index];
		const int32 LastStage = FMath::Max(Def->Stages.Num() - 1, 0);
		Rover->RestoreBlockRecord(PlacedIds[Index], EConstructionPhase::Complete, LastStage, 1.f, Def->MaxHealth, PlacedScalars[Index]);
	}
	Rover->MarkVisualsDirty();

	// Ready to drive: board with F; W/S drive, A/D steer.
	Rover->ControlMode = EPilotControlMode::Ground;
	SpawnedRover = Rover;

	UE_LOG(LogExoneer, Log, TEXT("TestRoverSpawner: rover ready (%d blocks)."), Rover->GetBlockCount());
}

void ATestRoverSpawner::StartAutomatedFlightProfile()
{
	if (!HasAuthority() || bFlightProfileActive || !GetWorld())
	{
		return;
	}

	AVehicleConstruct* Rover = SpawnedRover.Get();
	if (!Rover)
	{
		for (TActorIterator<AVehicleConstruct> It(GetWorld()); It; ++It)
		{
			Rover = *It;
			break;
		}
	}
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	APlayerSurvivalCharacter* Pilot = PC ? Cast<APlayerSurvivalCharacter>(PC->GetPawn()) : nullptr;
	if (!Rover || !Pilot || !Rover->OnInteract_Implementation(Pilot) || !Pilot->IsPiloting())
	{
		UE_LOG(LogExoneer, Error, TEXT("FLIGHT PROFILE FAIL: could not seat the local engineer in the test rover."));
		return;
	}

	// The character's normal 20 Hz packet stream would overwrite this fixture's
	// packets. The pawn remains attached and its camera remains active; only its
	// gameplay tick sleeps until the profile hands control back.
	Pilot->SetActorTickEnabled(false);
	Rover->ControlMode = EPilotControlMode::Flight;
	Rover->SetPilotInput(FPilotInput());
	SpawnedRover = Rover;
	FlightTestPilot = Pilot;
	bFlightProfileActive = true;
	FlightProfileTime = 0.f;
	FlightProfileLogTime = 0.f;
	FlightProfileMaxClimbMS = 0.f;
	FlightProfileHoverVerticalMS = 0.f;
	FlightProfileMaxForwardMS = 0.f;
	FlightProfileMaxBankDeg = 0.f;
	FlightProfileLevelBankDeg = 0.f;
	FlightProfileYawTravelDeg = 0.f;
	FlightProfileMinDescentMS = 0.f;
	FlightProfileMaxGyro01 = 0.f;
	FlightProfileMinEnergyKJ = TNumericLimits<float>::Max();
	FlightProfileLastYawDeg = Rover->GetActorRotation().Yaw;
	bFlightProfileInverted = false;
	SetActorTickEnabled(true);
	UE_LOG(LogExoneer, Display, TEXT("FLIGHT PROFILE START: fresh rover, 26 s real-physics sortie."));
}

void ATestRoverSpawner::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bFlightProfileActive)
	{
		return;
	}

	AVehicleConstruct* Rover = SpawnedRover.Get();
	APlayerSurvivalCharacter* Pilot = FlightTestPilot.Get();
	if (!Rover || !Pilot)
	{
		UE_LOG(LogExoneer, Error, TEXT("FLIGHT PROFILE FAIL: rover or pilot disappeared."));
		bFlightProfileActive = false;
		SetActorTickEnabled(false);
		return;
	}

	FlightProfileTime += DeltaSeconds;
	FPilotInput Input;
	const TCHAR* Phase = TEXT("RECOVER");
	if (FlightProfileTime < 3.f)
	{
		Input.Move.Z = 1.f;
		Phase = TEXT("CLIMB");
	}
	else if (FlightProfileTime < 7.f)
	{
		Phase = TEXT("HOVER");
	}
	else if (FlightProfileTime < 9.f)
	{
		Input.Move.X = 1.f;
		Phase = TEXT("FORWARD");
	}
	else if (FlightProfileTime < 11.f)
	{
		Phase = TEXT("BRAKE");
	}
	else if (FlightProfileTime < 12.5f)
	{
		Input.Rotate.Z = 1.f;
		Phase = TEXT("ROLL");
	}
	else if (FlightProfileTime < 16.f)
	{
		Phase = TEXT("LEVEL");
	}
	else if (FlightProfileTime < 18.f)
	{
		Input.Rotate.Y = 0.75f;
		Phase = TEXT("YAW");
	}
	else if (FlightProfileTime < 21.f)
	{
		Phase = TEXT("YAW RELEASE");
	}
	else if (FlightProfileTime < 23.f)
	{
		Input.HeldFlags = EPilotHeldFlags::Descend;
		Phase = TEXT("DESCEND");
	}
	Rover->SetPilotInput(Input);

	const FVehicleDrivetrainSummary D = Rover->GetDrivetrainSummary();
	const float HorizontalMS = Rover->GetVelocity().Size2D() / 100.f;
	const float YawDeg = Rover->GetActorRotation().Yaw;
	const float YawStep = FRotator::NormalizeAxis(YawDeg - FlightProfileLastYawDeg);
	FlightProfileLastYawDeg = YawDeg;
	if (FlightProfileTime >= 16.f && FlightProfileTime < 18.f)
	{
		FlightProfileYawTravelDeg += YawStep;
	}
	FlightProfileMaxClimbMS = FMath::Max(FlightProfileMaxClimbMS, D.VerticalSpeedMS);
	if (FlightProfileTime >= 6.5f && FlightProfileTime < 7.f)
	{
		FlightProfileHoverVerticalMS = D.VerticalSpeedMS;
	}
	if (FlightProfileTime >= 7.f && FlightProfileTime < 11.f)
	{
		FlightProfileMaxForwardMS = FMath::Max(FlightProfileMaxForwardMS, HorizontalMS);
	}
	if (FlightProfileTime >= 11.f && FlightProfileTime < 12.5f)
	{
		FlightProfileMaxBankDeg = FMath::Max(FlightProfileMaxBankDeg, FMath::Abs(D.BankDeg));
	}
	if (FlightProfileTime >= 15.5f && FlightProfileTime < 16.f)
	{
		FlightProfileLevelBankDeg = FMath::Abs(D.BankDeg);
	}
	if (FlightProfileTime >= 21.f && FlightProfileTime < 23.f)
	{
		FlightProfileMinDescentMS = FMath::Min(FlightProfileMinDescentMS, D.VerticalSpeedMS);
	}
	FlightProfileMaxGyro01 = FMath::Max(FlightProfileMaxGyro01, D.GyroSaturation01);
	FlightProfileMinEnergyKJ = FMath::Min(FlightProfileMinEnergyKJ, D.StoredEnergyWs / 1000.f);
	bFlightProfileInverted |= D.bLiftInverted;

	if (FlightProfileTime >= FlightProfileLogTime)
	{
		FlightProfileLogTime += 1.f;
		UE_LOG(LogExoneer, Display,
			TEXT("FLIGHT PROFILE t=%05.1f %-11s v=%5.2f vz=%+5.2f bank=%+5.1f pitch=%+5.1f yaw=%+6.1f TWR=%.2f lift=%.2f gyro=%.2f energy=%.0f kJ%s"),
			FlightProfileTime, Phase, HorizontalMS, D.VerticalSpeedMS, D.BankDeg, D.PitchDeg,
			YawDeg, D.AscentTwr, D.LiftFraction01, D.GyroSaturation01,
			D.StoredEnergyWs / 1000.f, D.bLiftInverted ? TEXT(" INVERTED") : TEXT(""));
	}

	if (FlightProfileTime < 26.f)
	{
		return;
	}

	Rover->SetPilotInput(FPilotInput());
	Pilot->SetActorTickEnabled(true);
	bFlightProfileActive = false;
	SetActorTickEnabled(false);
	const FVehicleDrivetrainSummary Final = Rover->GetDrivetrainSummary();
	const bool bPass = FlightProfileMaxClimbMS > 1.f
		&& FMath::Abs(FlightProfileHoverVerticalMS) < 0.75f
		&& FlightProfileMaxForwardMS > 2.f
		&& FlightProfileMaxBankDeg > 5.f && FlightProfileMaxBankDeg < 36.f
		&& FlightProfileLevelBankDeg < 3.f
		&& FMath::Abs(FlightProfileYawTravelDeg) > 10.f
		&& FlightProfileMinDescentMS > -4.f && FlightProfileMinDescentMS < -0.5f
		&& FlightProfileMaxGyro01 < 0.8f
		&& FlightProfileMinEnergyKJ > 250.f
		&& !bFlightProfileInverted
		&& FMath::Abs(Final.BankDeg) < 3.f && FMath::Abs(Final.PitchDeg) < 3.f;
	const FString Result = FString::Printf(
		TEXT("climb %.2f m/s; hover %+.2f m/s; forward %.2f m/s; bank %.1f -> %.1f deg; yaw %.1f deg; descend %.2f m/s; gyro %.0f%%; min energy %.0f kJ; final bank/pitch %+.1f/%+.1f"),
		FlightProfileMaxClimbMS, FlightProfileHoverVerticalMS, FlightProfileMaxForwardMS,
		FlightProfileMaxBankDeg, FlightProfileLevelBankDeg, FlightProfileYawTravelDeg,
		FlightProfileMinDescentMS, FlightProfileMaxGyro01 * 100.f, FlightProfileMinEnergyKJ,
		Final.BankDeg, Final.PitchDeg);
	if (bPass)
	{
		UE_LOG(LogExoneer, Display, TEXT("FLIGHT PROFILE PASS: %s"), *Result);
	}
	else
	{
		UE_LOG(LogExoneer, Error, TEXT("FLIGHT PROFILE FAIL: %s%s"), *Result,
			bFlightProfileInverted ? TEXT("; lift inverted") : TEXT(""));
	}
}

static void ExoneerFlightProfileCommand(const TArray<FString>&, UWorld* World)
{
	if (!World || World->GetNetMode() == NM_Client)
	{
		return;
	}
	for (TActorIterator<ATestRoverSpawner> It(World); It; ++It)
	{
		It->StartAutomatedFlightProfile();
		return;
	}
	UE_LOG(LogExoneer, Error, TEXT("FLIGHT PROFILE FAIL: no ATestRoverSpawner in %s."), *World->GetName());
}

static FAutoConsoleCommandWithWorldAndArgs GExoneerFlightProfileCmd(
	TEXT("exoneer.FlightProfile"),
	TEXT("Run the 26-second real-physics alpha flight profile on the test rover."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&ExoneerFlightProfileCommand));
