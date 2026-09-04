// Copyright Exoneer contributors.
#include "Player/PlayerSurvivalCharacter.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "Components/InventoryComponent.h"
#include "Components/SurvivalStatsComponent.h"
#include "Components/HealthComponent.h"
#include "Components/InteractionComponent.h"
#include "Components/MiningToolComponent.h"
#include "Components/BuildToolComponent.h"
#include "Components/CapsuleComponent.h"
#include "CableComponent.h"
#include "Machines/UmbilicalPortPiece.h"
#include "Maintenance/ExoneerMaintenance.h"
#include "Data/ItemDefinitionDataAsset.h"
#include "Data/PieceDefinitionDataAsset.h"
#include "Data/VehicleBlockDefinitionDataAsset.h"
#include "Engine/Engine.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Vehicles/VehicleConstruct.h"
#include "Vehicles/ExoneerVehicleUnits.h"
#include "Interfaces/Pilotable.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputActionValue.h"
#include "Components/InputComponent.h"
#include "GameFramework/PlayerInput.h"
#include "InputCoreTypes.h"
#include "Components/CraftingComponent.h"
#include "Components/InteractionComponent.h"
#include "Data/RecipeDefinitionDataAsset.h"
#include "Machines/MachinePiece.h"
#include "World/ProjectSubsystem.h"
#include "World/ExoneerGameState.h"
#include "Engine/AssetManager.h"
#include "Net/UnrealNetwork.h"
#include "Exoneer.h"

APlayerSurvivalCharacter::APlayerSurvivalCharacter()
{
	PrimaryActorTick.bCanEverTick = true;

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("FPCamera"));
	Camera->SetupAttachment(GetCapsuleComponent());
	Camera->SetRelativeLocation(FVector(0.f, 0.f, 70.f));
	Camera->bUsePawnControlRotation = true;

	// Chase boom for piloting. It hangs off the capsule at the same eye point
	// as the visor, and the capsule is welded to the cockpit block while
	// seated, so the boom inherits the construct's rotation directly - heading
	// on the ground, full attitude in flight. bUsePawnControlRotation is OFF
	// for exactly that reason: the control rotation is world-absolute and
	// would leave the view staring at a fixed compass bearing while the rover
	// turns under it. Free-look is applied as a SEAT-RELATIVE offset in Tick.
	ChaseArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("ChaseArm"));
	ChaseArm->SetupAttachment(GetCapsuleComponent());
	ChaseArm->SetRelativeLocation(FVector(0.f, 0.f, 70.f));
	ChaseArm->SetRelativeRotation(FRotator(ChaseArmPitchDeg, 0.f, 0.f));
	ChaseArm->TargetArmLength = ChaseArmDefaultCm;
	ChaseArm->SocketOffset = FVector(0.f, 0.f, ChaseCameraRiseCm);
	ChaseArm->bUsePawnControlRotation = false;
	ChaseArm->bInheritPitch = true;
	ChaseArm->bInheritYaw = true;
	ChaseArm->bInheritRoll = true;
	// Probe on the stock camera channel so terrain, slabs and structures push
	// the view in. The construct's own block boxes ignore that channel
	// (VehicleConstruct block-box setup), which is what keeps the boom from
	// collapsing onto the roof of the very vehicle it is filming.
	ChaseArm->bDoCollisionTest = true;
	ChaseArm->ProbeChannel = ECC_Camera;
	ChaseArm->ProbeSize = 20.f;
	// No lag: see ChaseCameraLagSpeed. Smoothing here would hide suspension
	// behaviour that is still being diagnosed.
	ChaseArm->bEnableCameraLag = false;
	ChaseArm->bEnableCameraRotationLag = false;
	DesiredChaseArmCm = ChaseArmDefaultCm;

	ChaseCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("ChaseCamera"));
	ChaseCamera->SetupAttachment(ChaseArm, USpringArmComponent::SocketName);
	ChaseCamera->bUsePawnControlRotation = false;
	// Exactly one camera may be active: APawn::CalcCamera takes the first
	// ACTIVE UCameraComponent it finds, so the chase camera starts asleep and
	// the visor owns the view until a cockpit takes it.
	ChaseCamera->SetAutoActivate(false);

	bUseControllerRotationYaw = true;
	if (UCharacterMovementComponent* CMC = GetCharacterMovement())
	{
		CMC->bOrientRotationToMovement = false;
		CMC->MaxWalkSpeed = WalkSpeed;
		CMC->JumpZVelocity = JumpHeight;
		CMC->AirControl = 0.5f;
		// A walking engineer is a ~2 kN shove, full stop. The old value
		// (80000 SCALED TO MASS) applied ~80 g to anything touched - it
		// launched a 1.6 t rover into the air on contact. Unscaled, the same
		// fixed force barely rolls a heavy vehicle (F/m) and still nudges
		// small debris, which is the physical outcome.
		CMC->PushForceFactor = 200000.f;   // kg*cm/s^2 = 2 kN
		CMC->bPushForceScaledToMass = false;
		CMC->bScalePushForceToVelocity = true;
		// No touch IMPULSE at all: the engine default (500k) fires on first
		// contact scaled by pawn velocity, so jumping onto your own rover's
		// wheel kicked the whole vehicle. Sustained pushing (above) and the
		// standing weight transfer below are the physical channels.
		CMC->InitialPushForceFactor = 0.f;
		// Standing weight transfer (the suspension dipping under you) rides
		// the engine's default standing downward force - no override needed.
	}

	// A body is not terrain: the wheel suspension probe must never see this
	// capsule. It traces DOWN from above each wheel, so standing on or beside
	// a wheel otherwise put the capsule between the ray start and the ground -
	// the suspension read "ground is right here", slammed to full compression
	// and fired the spring, launching the wheel and the engineer with it (and
	// because the fake ground tracked the pawn, it felt magnetic).
	if (UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		Capsule->SetCollisionResponseToChannel(ECC_WheelProbe, ECR_Ignore);
	}

	Inventory     = CreateDefaultSubobject<UInventoryComponent>(TEXT("Inventory"));
	Inventory->bUseWeight = true;
	Inventory->MaxCapacity = 120.f;

	Survival      = CreateDefaultSubobject<USurvivalStatsComponent>(TEXT("Survival"));
	HealthC       = CreateDefaultSubobject<UHealthComponent>(TEXT("Health"));
	Interactor    = CreateDefaultSubobject<UInteractionComponent>(TEXT("Interactor"));
	MiningTool    = CreateDefaultSubobject<UMiningToolComponent>(TEXT("MiningTool"));
	BuildTool     = CreateDefaultSubobject<UBuildToolComponent>(TEXT("BuildTool"));

	// Chest offset on the capsule: the default engineer has no mesh socket.
	UmbilicalCable = CreateDefaultSubobject<UCableComponent>(TEXT("UmbilicalCable"));
	UmbilicalCable->SetupAttachment(GetCapsuleComponent());
	UmbilicalCable->SetRelativeLocation(FVector(20.f, 0.f, 30.f));
	UmbilicalCable->CableLength = 800.f;
	UmbilicalCable->NumSegments = 10;
	UmbilicalCable->CableWidth = 4.f;
	UmbilicalCable->bAttachStart = true;
	UmbilicalCable->bAttachEnd = true;
	UmbilicalCable->SetVisibility(false);
	UmbilicalCable->SetHiddenInGame(true);
}

void APlayerSurvivalCharacter::BeginPlay()
{
	Super::BeginPlay();

	// Prototype bootstrap: grant the starter kit once, server-side.
	if (HasAuthority() && Inventory)
	{
		for (const FInventoryEntry& Entry : StarterItems)
		{
			if (UItemDefinitionDataAsset* Item = Entry.Item.LoadSynchronous())
			{
				Inventory->AddItem(Item, Entry.Count);
			}
		}
	}

	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
			ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
		{
			if (DefaultMappingContext)
			{
				Subsystem->AddMappingContext(DefaultMappingContext, 0);
			}
		}
	}
}

void APlayerSurvivalCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// --- Chase view: seat-relative aim, and the wheel's zoom target ---
	if (ChaseArm && IsLocallyControlled())
	{
		// The wheel moves a TARGET, never the boom itself: a wheel axis fires
		// once per frame while it spins, and writing the length directly made
		// the view jump a whole notch per frame.
		ChaseArm->TargetArmLength = FMath::FInterpTo(
			ChaseArm->TargetArmLength, DesiredChaseArmCm, DeltaSeconds, FMath::Max(ChaseZoomInterpSpeed, 0.1f));

		const bool bLag = ChaseCameraLagSpeed > KINDA_SMALL_NUMBER;
		ChaseArm->bEnableCameraLag = bLag;
		ChaseArm->CameraLagSpeed = bLag ? ChaseCameraLagSpeed : 0.f;

		if (bSeatViewActive)
		{
			// Free-look while driving is the mouse travel SINCE seating, kept
			// in seat space. The control rotation still does the accumulating
			// and the pitch clamping, so the sensitivity on the ground is the
			// same one the engineer has on foot; only the frame of reference
			// changes. In Flight the mouse is the attitude stick (Input_Look
			// returns early), so this offset simply holds where it was.
			FRotator LookOffset = FRotator::ZeroRotator;
			if (Controller)
			{
				const FRotator Control = Controller->GetControlRotation();
				LookOffset.Yaw = FRotator::NormalizeAxis(Control.Yaw - SeatLookReference.Yaw);
				LookOffset.Pitch = FRotator::NormalizeAxis(Control.Pitch - SeatLookReference.Pitch);
			}
			ChaseArm->SetRelativeRotation(FRotator(ChaseArmPitchDeg + LookOffset.Pitch, LookOffset.Yaw, 0.f));
		}
	}

	// While seated, ship the pilot packet at ~PilotInputSendHz. A steady
	// stream (zero axes included) lets the construct stop cleanly when keys
	// are released; the RPC is unreliable, the next send supersedes. The
	// server HOLDS the last packet between sends (no decay), so axes are
	// per-window samples while held states and the mode-toggle counter are
	// sampled fresh at send time and survive packet timing.
	if (PilotedConstruct && IsLocallyControlled())
	{
		PilotSendAccumulator += DeltaSeconds;
		const float SendInterval = 1.f / FMath::Max(PilotInputSendHz, 1.f);
		if (PilotSendAccumulator >= SendInterval)
		{
			const float Window = PilotSendAccumulator;
			PilotSendAccumulator = 0.f;
			FPilotInput Packet = PendingPilotInput;
			const bool bFlying = PilotedConstruct->GetControlMode() == EPilotControlMode::Flight;

			// Mouse -> bounded RATE command. The whole window is integrated and
			// divided by its own duration, so the packet carries counts per
			// second: independent of frame rate, of send rate, and of whether
			// the mouse happened to be still on the last frame.
			if (bFlying)
			{
				const float Scale = FMath::Max(PilotLookCountsPerSecFullRate, 1.f) * FMath::Max(Window, KINDA_SMALL_NUMBER);
				Packet.Rotate.X = FMath::Clamp(static_cast<float>(-PendingLookCounts.Y) / Scale, -1.f, 1.f);
				Packet.Rotate.Y = FMath::Clamp(static_cast<float>(PendingLookCounts.X) / Scale, -1.f, 1.f);
			}

			// Lift is up / down / hold, and the up and down keys are the pair
			// every pilot already knows: Move.Z is the CLIMB level, 1 while the
			// lift key is held; the handbrake key becomes DESCEND in flight.
			// One key each way, nothing latched, and released means the server
			// holds altitude rather than picking one of climb or fall.
			//
			// Handbrake stays a GROUND control, so the same key cannot slam the
			// parking brake on at altitude.
			Packet.Move.Z = (bFlying && bLiftHeld) ? 1.f : 0.f;
			Packet.Brake = bBrakeHeld ? 1.f : 0.f;
			Packet.HeldFlags = 0;
			if (bHandbrakeHeld)
			{
				Packet.HeldFlags |= bFlying ? EPilotHeldFlags::Descend : EPilotHeldFlags::Handbrake;
			}
			Packet.ModeToggleCount = ModeTogglePressCounter & 0x3;
			Server_SendPilotInput(PilotedConstruct, Packet);
			PendingPilotInput = FPilotInput();
			PendingLookCounts = FVector2D::ZeroVector;
		}
	}
}

void APlayerSurvivalCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(APlayerSurvivalCharacter, PilotedConstruct);
	DOREPLIFETIME(APlayerSurvivalCharacter, UmbilicalSource);
	DOREPLIFETIME(APlayerSurvivalCharacter, UmbilicalDetachedAt);
}

void APlayerSurvivalCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (HasAuthority() && UmbilicalSource)
	{
		SetUmbilicalSource(nullptr, /*bRangeBreak*/ false);
	}
	Super::EndPlay(EndPlayReason);
}

void APlayerSurvivalCharacter::SetUmbilicalSource(AUmbilicalPortPiece* Port, bool bRangeBreak)
{
	if (!HasAuthority())
	{
		return;
	}
	if (UmbilicalSource == Port)
	{
		return;
	}
	if (AUmbilicalPortPiece* Previous = UmbilicalSource.Get())
	{
		Previous->NotifyPawnDisconnected(this);
	}
	UmbilicalSource = Port;
	if (!Port && bRangeBreak && GetWorld())
	{
		UmbilicalDetachedAt = GetWorld()->GetTimeSeconds();
	}
	else if (Port)
	{
		UmbilicalDetachedAt = -1.f;
	}
	UpdateUmbilicalCable();
}

void APlayerSurvivalCharacter::OnRep_UmbilicalSource()
{
	UpdateUmbilicalCable();
}

void APlayerSurvivalCharacter::UpdateUmbilicalCable()
{
	if (!UmbilicalCable)
	{
		return;
	}
	AUmbilicalPortPiece* Port = UmbilicalSource.Get();
	const bool bLinked = IsValid(Port);
	UmbilicalCable->SetVisibility(bLinked);
	UmbilicalCable->SetHiddenInGame(!bLinked);
	if (bLinked)
	{
		UmbilicalCable->SetAttachEndTo(Port, NAME_None);
		UmbilicalCable->CableLength = Port->UmbilicalLengthCm;
	}
	else
	{
		UmbilicalCable->SetAttachEndTo(nullptr, NAME_None);
	}
}

void APlayerSurvivalCharacter::Landed(const FHitResult& Hit)
{
	Super::Landed(Hit);
	const float SpeedMps = FMath::Abs(FVector::DotProduct(GetVelocity(), Hit.ImpactNormal)) / 100.f;
	if (HasAuthority())
	{
		ApplyLandingSealWear(SpeedMps);
	}
	else
	{
		Server_ReportLanding(SpeedMps);
	}
}

void APlayerSurvivalCharacter::ApplyLandingSealWear(float ImpactSpeedMps)
{
	if (!Survival)
	{
		return;
	}
	const float Delta = ExoneerMaintenance::SealLandingLeakDeltaLps(ImpactSpeedMps);
	if (Delta > 0.f)
	{
		Survival->AddLeakRateLps(Delta);
	}
}

bool APlayerSurvivalCharacter::Server_ReportLanding_Validate(float ImpactSpeedMps)
{
	return ImpactSpeedMps >= 0.f && ImpactSpeedMps < 200.f;
}

void APlayerSurvivalCharacter::Server_ReportLanding_Implementation(float ImpactSpeedMps)
{
	ApplyLandingSealWear(ImpactSpeedMps);
}

void APlayerSurvivalCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	// Register the mapping context HERE, not only in BeginPlay: on the first
	// possession the pawn's BeginPlay can run before the controller is set,
	// in which case the BeginPlay registration silently does nothing and
	// every key stays dead. This path always has a controller.
	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
			ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
		{
			if (DefaultMappingContext && !Subsystem->HasMappingContext(DefaultMappingContext))
			{
				Subsystem->AddMappingContext(DefaultMappingContext, 0);
			}
		}
	}

	UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!EIC) return;

	auto Bind = [&](UInputAction* IA, ETriggerEvent E, auto Fn)
	{
		if (IA) EIC->BindAction(IA, E, this, Fn);
	};

	// Continuous inputs stay on Triggered (fires every frame while held);
	// one-shot actions use Started, or holding the key repeats them per tick
	// (holding B used to machine-gun through the whole quick bar).
	Bind(IA_Move,             ETriggerEvent::Triggered,  &APlayerSurvivalCharacter::Input_Move);
	Bind(IA_Look,             ETriggerEvent::Triggered,  &APlayerSurvivalCharacter::Input_Look);
	Bind(IA_Jump,             ETriggerEvent::Triggered,  &APlayerSurvivalCharacter::Input_Jump);   // held = raise lift collective
	Bind(IA_Jump,             ETriggerEvent::Completed,  &APlayerSurvivalCharacter::Input_JumpStop);
	Bind(IA_Sprint,           ETriggerEvent::Started,    &APlayerSurvivalCharacter::Input_SprintStart);
	Bind(IA_Sprint,           ETriggerEvent::Completed,  &APlayerSurvivalCharacter::Input_SprintStop);
	Bind(IA_Crouch,           ETriggerEvent::Started,    &APlayerSurvivalCharacter::Input_CrouchToggle);
	Bind(IA_Interact,         ETriggerEvent::Started,    &APlayerSurvivalCharacter::Input_Interact);
	Bind(IA_PrimaryAction,    ETriggerEvent::Started,    &APlayerSurvivalCharacter::Input_PrimaryStart);
	Bind(IA_PrimaryAction,    ETriggerEvent::Completed,  &APlayerSurvivalCharacter::Input_PrimaryStop);
	Bind(IA_SecondaryAction,  ETriggerEvent::Started,    &APlayerSurvivalCharacter::Input_SecondaryStart);
	Bind(IA_SecondaryAction,  ETriggerEvent::Completed,  &APlayerSurvivalCharacter::Input_SecondaryStop);
	Bind(IA_OpenInventory,    ETriggerEvent::Started,    &APlayerSurvivalCharacter::Input_OpenInventory);
	Bind(IA_OpenBuildMenu,    ETriggerEvent::Started,    &APlayerSurvivalCharacter::Input_OpenBuildMenu);
	Bind(IA_CycleVehicleBlock, ETriggerEvent::Started,   &APlayerSurvivalCharacter::Input_CycleVehicleBlock);
	Bind(IA_RotateBlock,      ETriggerEvent::Started,    &APlayerSurvivalCharacter::Input_RotateBlock);
	Bind(IA_ConfirmPlace,     ETriggerEvent::Started,    &APlayerSurvivalCharacter::Input_ConfirmPlace);
	Bind(IA_CancelPlace,      ETriggerEvent::Started,    &APlayerSurvivalCharacter::Input_CancelPlace);
	Bind(IA_ToggleTool,       ETriggerEvent::Started,    &APlayerSurvivalCharacter::Input_ToggleTool);
	Bind(IA_EnterExitCockpit, ETriggerEvent::Started,    &APlayerSurvivalCharacter::Input_EnterExitCockpit);
	Bind(IA_Brake,            ETriggerEvent::Started,    &APlayerSurvivalCharacter::Input_BrakeStart);
	Bind(IA_Brake,            ETriggerEvent::Completed,  &APlayerSurvivalCharacter::Input_BrakeStop);
	Bind(IA_Handbrake,        ETriggerEvent::Started,    &APlayerSurvivalCharacter::Input_HandbrakeStart);
	Bind(IA_Handbrake,        ETriggerEvent::Completed,  &APlayerSurvivalCharacter::Input_HandbrakeStop);
	Bind(IA_ToggleControlMode, ETriggerEvent::Started,   &APlayerSurvivalCharacter::Input_ToggleControlMode);
	// The wheel is an axis: Triggered fires on every notch while it spins.
	Bind(IA_CameraZoom,       ETriggerEvent::Triggered,  &APlayerSurvivalCharacter::Input_CameraZoom);

	const FKey DigitKeys[8] = { EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four,
		EKeys::Five, EKeys::Six, EKeys::Seven, EKeys::Eight };
	for (int32 i = 0; i < 8; ++i)
	{
		FInputKeyBinding BindDigit(FInputChord(DigitKeys[i]), IE_Pressed);
		BindDigit.KeyDelegate.GetDelegateForManualSet().BindLambda([this, i]() { EnqueueFocusedRecipe(i); });
		PlayerInputComponent->KeyBindings.Add(MoveTemp(BindDigit));
	}
	const FKey ProjectKeys[3] = { EKeys::J, EKeys::K, EKeys::L };
	for (int32 i = 0; i < 3; ++i)
	{
		FInputKeyBinding BindProj(FInputChord(ProjectKeys[i]), IE_Pressed);
		BindProj.KeyDelegate.GetDelegateForManualSet().BindLambda([this, i]() { ToggleProjectByIndex(i); });
		PlayerInputComponent->KeyBindings.Add(MoveTemp(BindProj));
	}
}

void APlayerSurvivalCharacter::Input_Move(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	if (PilotedConstruct)
	{
		if (PilotedConstruct->GetControlMode() == EPilotControlMode::Ground)
		{
			// Ground driving: W/S is drive throttle, A/D is steer.
			PendingPilotInput.Throttle = Axis.Y;
			PendingPilotInput.Steer = Axis.X;
			return;
		}
		// Flight: W/S is forward/back thrust, A/D is ROLL through the gyro.
		// A/D used to command lateral thrust, which does nothing on a craft
		// with no side-facing thrusters - it felt like the controls were dead.
		// Roll plus pitch is how you actually steer a thruster craft.
		PendingPilotInput.Move.X = Axis.Y;
		PendingPilotInput.Rotate.Z = Axis.X;
		return;
	}
	if (!Controller) return;
	const FRotator YawRot(0.f, Controller->GetControlRotation().Yaw, 0.f);
	const FVector Fwd = FRotationMatrix(YawRot).GetUnitAxis(EAxis::X);
	const FVector Right = FRotationMatrix(YawRot).GetUnitAxis(EAxis::Y);
	AddMovementInput(Fwd, Axis.Y);
	AddMovementInput(Right, Axis.X);
}

void APlayerSurvivalCharacter::Input_Look(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
	if (PilotedConstruct && PilotedConstruct->GetControlMode() == EPilotControlMode::Flight)
	{
		// Flight: the mouse is a RATE command, so accumulate the whole send
		// window and let Tick turn it into counts per second. Writing the raw
		// per-frame delta straight into the intent made the mouse a torque
		// switch with no proportional region.
		PendingLookCounts += Axis;
		return;
	}
	// On foot, and Ground-mode piloting: the mouse is a free-look camera. While
	// seated the control rotation is no longer the view - Tick reads it as an
	// offset FROM the seat - but it stays the accumulator, so the sensitivity
	// and the pitch clamp are identical on foot and in the cockpit.
	AddControllerYawInput(Axis.X);
	AddControllerPitchInput(-Axis.Y);
}

void APlayerSurvivalCharacter::Input_Jump(const FInputActionValue&)
{
	if (PilotedConstruct)
	{
		// Held = lift. The held state is sampled at send time so a lost frame
		// cannot drop it, and releasing the key closes the valve.
		bLiftHeld = true;
		return;
	}
	Jump();
}

void APlayerSurvivalCharacter::Input_JumpStop(const FInputActionValue&)
{
	bLiftHeld = false;
}

void APlayerSurvivalCharacter::Input_SprintStart(const FInputActionValue&)
{
	if (auto* CMC = GetCharacterMovement()) CMC->MaxWalkSpeed = SprintSpeed;
}

void APlayerSurvivalCharacter::Input_SprintStop(const FInputActionValue&)
{
	if (auto* CMC = GetCharacterMovement()) CMC->MaxWalkSpeed = WalkSpeed;
}

void APlayerSurvivalCharacter::Input_CrouchToggle(const FInputActionValue&)
{
	if (PilotedConstruct) return;
	if (bIsCrouched) UnCrouch(); else Crouch();
}

void APlayerSurvivalCharacter::Input_Interact(const FInputActionValue&)
{
	if (Interactor) Interactor->TryInteract();
}

void APlayerSurvivalCharacter::Input_PrimaryStart(const FInputActionValue&)
{
	if (PilotedConstruct) return;   // Tools stay holstered while seated.
	switch (ToolMode)
	{
	case EPlayerToolMode::Mining: if (MiningTool) MiningTool->SetMiningActive(true); break;
	case EPlayerToolMode::Build:  if (BuildTool)  BuildTool->TryConfirmPlacement(); break;
	case EPlayerToolMode::Weld:   if (BuildTool)  BuildTool->SetWeldActive(true); break;
	default: break;
	}
}

void APlayerSurvivalCharacter::Input_PrimaryStop(const FInputActionValue&)
{
	if (MiningTool) MiningTool->SetMiningActive(false);
	if (BuildTool)  BuildTool->SetWeldActive(false);
}

void APlayerSurvivalCharacter::Input_SecondaryStart(const FInputActionValue&)
{
	if (PilotedConstruct) return;
	if ((ToolMode == EPlayerToolMode::Build || ToolMode == EPlayerToolMode::Weld) && BuildTool)
	{
		BuildTool->SetDeconstructActive(true);
	}
}

void APlayerSurvivalCharacter::Input_SecondaryStop(const FInputActionValue&)
{
	if (BuildTool) BuildTool->SetDeconstructActive(false);
}

void APlayerSurvivalCharacter::Input_OpenInventory(const FInputActionValue&)
{
	RequestOpenInventoryUI();
}

void APlayerSurvivalCharacter::Input_OpenBuildMenu(const FInputActionValue&)
{
	// Debug quick bar: until the diegetic UI stack exists, the build-menu key
	// cycles the PIECES and arms the build tool directly.
	CycleQuickBar(/*bVehicleBlocks*/ false);
	RequestOpenBuildMenuUI();
}

void APlayerSurvivalCharacter::Input_CycleVehicleBlock(const FInputActionValue&)
{
	CycleQuickBar(/*bVehicleBlocks*/ true);
}

/**
 * Walk the quick bar to the next entry of ONE kind and arm it. The list holds
 * pieces and vehicle blocks together, and it has grown past the point where a
 * single cycle key was usable: B now walks the pieces and N the blocks, each
 * with its own cursor, so switching kinds does not lose your place in either.
 */
void APlayerSurvivalCharacter::CycleQuickBar(bool bVehicleBlocks)
{
	const int32 Count = QuickBar.Num();
	if (Count == 0 || !BuildTool)
	{
		return;
	}

	int32& Cursor = bVehicleBlocks ? BlockQuickBarIndex : PieceQuickBarIndex;
	for (int32 Step = 1; Step <= Count; ++Step)
	{
		// Cursor starts at INDEX_NONE, so the first press lands on entry 0.
		const int32 Candidate = (Cursor + Step + Count) % Count;
		UPrimaryDataAsset* Entry = QuickBar[Candidate];
		if (bVehicleBlocks)
		{
			if (UVehicleBlockDefinitionDataAsset* Block = Cast<UVehicleBlockDefinitionDataAsset>(Entry))
			{
				SetSelectedVehicleBlock(Block);
				Cursor = Candidate;
				QuickBarIndex = Candidate;
				break;
			}
		}
		else if (UPieceDefinitionDataAsset* Piece = Cast<UPieceDefinitionDataAsset>(Entry))
		{
			SetSelectedPiece(Piece);
			Cursor = Candidate;
			QuickBarIndex = Candidate;
			break;
		}
	}

	// A quick bar holding no entry of the wanted kind arms nothing rather than
	// dropping the player into build mode with a stale selection.
	if (Cursor == INDEX_NONE)
	{
		return;
	}
	BuildTool->SetBuildModeEnabled(true);
	ToolMode = EPlayerToolMode::Build;
}

void APlayerSurvivalCharacter::Input_RotateBlock(const FInputActionValue&)
{
	if (BuildTool) BuildTool->CycleOrientation(1);
}

void APlayerSurvivalCharacter::Input_ConfirmPlace(const FInputActionValue&)
{
	if (PilotedConstruct) return;
	if (BuildTool) BuildTool->TryConfirmPlacement();
}

void APlayerSurvivalCharacter::Input_CancelPlace(const FInputActionValue&)
{
	if (BuildTool)
	{
		BuildTool->SetBuildModeEnabled(false);
		BuildTool->SetWeldActive(false);
		BuildTool->SetDeconstructActive(false);
	}
	ToolMode = EPlayerToolMode::Mining;
}

void APlayerSurvivalCharacter::Input_ToggleTool(const FInputActionValue&)
{
	CycleToolMode();
}

void APlayerSurvivalCharacter::Input_EnterExitCockpit(const FInputActionValue&)
{
	if (PilotedConstruct)
	{
		Server_ExitPilot();
	}
	else if (Interactor)
	{
		// Interacting with a cockpit seats the pawn (IInteractable pipeline).
		Interactor->TryInteract();
	}
}

void APlayerSurvivalCharacter::Input_BrakeStart(const FInputActionValue&)   { bBrakeHeld = true; }
void APlayerSurvivalCharacter::Input_BrakeStop(const FInputActionValue&)    { bBrakeHeld = false; }
void APlayerSurvivalCharacter::Input_HandbrakeStart(const FInputActionValue&) { bHandbrakeHeld = true; }
void APlayerSurvivalCharacter::Input_HandbrakeStop(const FInputActionValue&)  { bHandbrakeHeld = false; }

void APlayerSurvivalCharacter::Input_ToggleControlMode(const FInputActionValue&)
{
	if (PilotedConstruct)
	{
		// Rolling counter: the press latches even if it lands between sends.
		ModeTogglePressCounter = (ModeTogglePressCounter + 1) & 0x3;
	}
}

void APlayerSurvivalCharacter::Input_CameraZoom(const FInputActionValue& Value)
{
	// Wheel up (positive) pulls the boom in. Clamped at both ends; the target
	// is remembered for the session, so re-boarding keeps the framing the
	// engineer chose rather than resetting it under him.
	const float Notches = Value.Get<float>();
	if (FMath::IsNearlyZero(Notches))
	{
		return;
	}
	DesiredChaseArmCm = FMath::Clamp(
		DesiredChaseArmCm - Notches * ChaseZoomStepCm, ChaseArmMinCm, ChaseArmMaxCm);
}

void APlayerSurvivalCharacter::GetActorEyesViewPoint(FVector& OutLocation, FRotator& OutRotation) const
{
	// The visor, never "whichever camera component was found first". The
	// rotation comes from the view rotation rather than the camera component
	// because a deactivated camera stops being re-oriented and its component
	// rotation goes stale.
	OutLocation = Camera ? Camera->GetComponentLocation() : GetPawnViewLocation();
	OutRotation = GetViewRotation();
}

void APlayerSurvivalCharacter::OnRep_PilotedConstruct()
{
	UpdateSeatView();
}

void APlayerSurvivalCharacter::UpdateSeatView()
{
	const bool bSeated = PilotedConstruct != nullptr;
	if (bSeated == bSeatViewActive)
	{
		return;
	}
	bSeatViewActive = bSeated;

	// The capsule must stop chasing the control rotation while seated, or it
	// fights the cockpit attachment every frame and the boom hanging off it
	// shears against the hull. Off in the seat, back on when standing.
	bUseControllerRotationYaw = !bSeated;

	if (bSeated)
	{
		// Board looking where the vehicle points: the free-look offset is
		// measured from here, so it starts at zero.
		SeatLookReference = Controller ? Controller->GetControlRotation() : GetActorRotation();
		if (ChaseArm)
		{
			ChaseArm->SetRelativeRotation(FRotator(ChaseArmPitchDeg, 0.f, 0.f));
		}
	}
	else
	{
		// Stand up straight. The seat carried the craft's full attitude into
		// the capsule, and only the YAW of that comes back under the
		// controller - a pawn stepping off a rolled rover would otherwise keep
		// standing at that roll, capsule and all.
		const FRotator Standing(0.f, GetActorRotation().Yaw, 0.f);
		SetActorRotation(Standing);
		if (Controller && IsLocallyControlled())
		{
			// Step out facing the way the pilot was looking, horizon level.
			const FRotator Look = ChaseArm ? ChaseArm->GetComponentRotation() : Controller->GetControlRotation();
			Controller->SetControlRotation(FRotator(0.f, Look.Yaw, 0.f));
		}
	}

	if (IsLocallyControlled())
	{
		// Exactly one active camera - see the constructor.
		if (ChaseCamera) ChaseCamera->SetActive(bSeated);
		if (Camera)      Camera->SetActive(!bSeated);
	}

	// A pilot aims nothing: the tools are already holstered while seated, so
	// the focus ray would only latch onto the rover's own hull and draw an
	// interaction prompt for the vehicle being driven.
	if (Interactor)
	{
		if (bSeated)
		{
			Interactor->ClearFocus();
		}
		Interactor->SetComponentTickEnabled(!bSeated);
	}
}

void APlayerSurvivalCharacter::CycleToolMode()
{
	switch (ToolMode)
	{
	case EPlayerToolMode::None:   ToolMode = EPlayerToolMode::Mining; break;
	case EPlayerToolMode::Mining: ToolMode = EPlayerToolMode::Build;  break;
	case EPlayerToolMode::Build:  ToolMode = EPlayerToolMode::Weld;   break;
	case EPlayerToolMode::Weld:   ToolMode = EPlayerToolMode::Mining; break;
	}
	if (BuildTool)
	{
		BuildTool->SetBuildModeEnabled(ToolMode == EPlayerToolMode::Build);
		BuildTool->SetWeldActive(false);
		BuildTool->SetDeconstructActive(false);
	}
	if (MiningTool) MiningTool->SetMiningActive(false);
}

void APlayerSurvivalCharacter::SetSelectedPiece(UPieceDefinitionDataAsset* Piece)
{
	if (BuildTool) BuildTool->SetSelectedPiece(Piece);
}

void APlayerSurvivalCharacter::SetSelectedVehicleBlock(UVehicleBlockDefinitionDataAsset* Block)
{
	if (BuildTool) BuildTool->SetSelectedVehicleBlock(Block);
}

// ---------------------------------------------------------------------------
// Piloting
// ---------------------------------------------------------------------------

void APlayerSurvivalCharacter::SetPilotedConstruct(AVehicleConstruct* Construct)
{
	if (!HasAuthority())
	{
		UE_LOG(LogExoneer, Warning, TEXT("SetPilotedConstruct is server-only."));
		return;
	}
	PilotedConstruct = Construct;

	// Reset the local intent so a listen host does not carry stale input
	// across seatings; remote clients reset when their next send fires. The
	// mode-toggle counter deliberately survives: the construct adopts its
	// current value on the first packet after a fresh seating.
	PendingPilotInput = FPilotInput();
	PilotSendAccumulator = 0.f;
	// Mouse counts banked in the partial window before an unseat would be
	// divided by the fresh window on re-boarding - up to a 2x rate spike on the
	// first packet, at exactly the moment the pilot is retaking control. And a
	// held key surviving an unseat means boarding with the lift key down opens
	// the valve before the pilot has asked for anything.
	PendingLookCounts = FVector2D::ZeroVector;
	bLiftHeld = false;
	bHandbrakeHeld = false;
	bBrakeHeld = false;

	// Directly, not through the OnRep: a listen host never gets an OnRep for
	// its own pawn, and would board in first person while every remote client
	// swapped to the chase boom.
	UpdateSeatView();
}

bool APlayerSurvivalCharacter::Server_SendPilotInput_Validate(AVehicleConstruct* Construct, FPilotInput Input)
{
	// Never gate on pilot state here: unreliable input packets race every
	// cockpit exit (client keeps streaming for >= 1 RTT after the server
	// unseated it), and a _Validate failure DISCONNECTS the client. The
	// implementation already drops input that no longer matches the seat.
	// Reject only what an honest client can never send: non-finite values.
	return !Input.ContainsNaN();
}

void APlayerSurvivalCharacter::Server_SendPilotInput_Implementation(AVehicleConstruct* Construct, FPilotInput Input)
{
	if (IsValid(Construct) && PilotedConstruct == Construct)
	{
		// SetPilotInput sanitizes every axis to its documented range.
		Construct->SetPilotInput(Input);
	}
}

bool APlayerSurvivalCharacter::Server_ExitPilot_Validate()
{
	// State can race with a server-side exit; never kick the client for it.
	return true;
}

void APlayerSurvivalCharacter::Server_ExitPilot_Implementation()
{
	if (PilotedConstruct)
	{
		IPilotable::Execute_ExitPilot(PilotedConstruct, this);
	}
}

// ---------------------------------------------------------------------------
// IDamageable
// ---------------------------------------------------------------------------

float APlayerSurvivalCharacter::ApplyExoneerDamage_Implementation(float Amount, EExoneerDamageType Type, AActor* DamageInstigator)
{
	return HealthC ? HealthC->ApplyDamage(Amount, Type, DamageInstigator) : 0.f;
}

float APlayerSurvivalCharacter::GetCurrentHealth_Implementation() const
{
	return HealthC ? HealthC->Health : 0.f;
}

float APlayerSurvivalCharacter::GetMaxHealth_Implementation() const
{
	return HealthC ? HealthC->MaxHealth : 100.f;
}

void APlayerSurvivalCharacter::EnqueueFocusedRecipe(int32 Index)
{
	if (PilotedConstruct || !Interactor)
	{
		return;
	}
	AActor* Focus = Interactor->GetFocusedActor();
	if (!Focus)
	{
		return;
	}
	UCraftingComponent* Crafting = Focus->FindComponentByClass<UCraftingComponent>();
	if (!Crafting)
	{
		return;
	}

	TArray<FPrimaryAssetId> Ids;
	UAssetManager::Get().GetPrimaryAssetIdList(FPrimaryAssetType(TEXT("Recipe")), Ids);
	TArray<URecipeDefinitionDataAsset*> Matches;
	for (const FPrimaryAssetId& Id : Ids)
	{
		URecipeDefinitionDataAsset* Recipe = Cast<URecipeDefinitionDataAsset>(UAssetManager::Get().GetPrimaryAssetObject(Id));
		if (!Recipe)
		{
			TSoftObjectPtr<URecipeDefinitionDataAsset> Soft(UAssetManager::Get().GetPrimaryAssetPath(Id));
			Recipe = Soft.LoadSynchronous();
		}
		if (Recipe && Recipe->Station == Crafting->StationType)
		{
			Matches.Add(Recipe);
		}
	}
	if (!Matches.IsValidIndex(Index))
	{
		return;
	}
	Interactor->RequestEnqueueRecipe(Crafting, Matches[Index]);
}

void APlayerSurvivalCharacter::ToggleProjectByIndex(int32 Index)
{
	if (HasAuthority())
	{
		Server_ToggleProject_Implementation(Index);
		return;
	}
	Server_ToggleProject(Index);
}

bool APlayerSurvivalCharacter::Server_ToggleProject_Validate(int32 Index)
{
	return Index >= 0 && Index < 16;
}

void APlayerSurvivalCharacter::Server_ToggleProject_Implementation(int32 Index)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	UProjectSubsystem* Projects = World->GetSubsystem<UProjectSubsystem>();
	AExoneerGameState* GS = World->GetGameState<AExoneerGameState>();
	if (!Projects || !GS || !GS->Projects.IsValidIndex(Index))
	{
		return;
	}
	const FProjectRuntime& Runtime = GS->Projects[Index];
	if (Runtime.State == EProjectState::Active)
	{
		Projects->AbandonProject(Runtime.ProjectId);
	}
	else
	{
		Projects->AcceptProject(Runtime.ProjectId);
	}
}
