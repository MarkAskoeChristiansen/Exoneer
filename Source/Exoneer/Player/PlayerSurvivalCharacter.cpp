// Copyright Exoneer contributors.
#include "Player/PlayerSurvivalCharacter.h"
#include "Camera/CameraComponent.h"
#include "Components/InventoryComponent.h"
#include "Components/SurvivalStatsComponent.h"
#include "Components/HealthComponent.h"
#include "Components/InteractionComponent.h"
#include "Components/MiningToolComponent.h"
#include "Components/BuildToolComponent.h"
#include "Components/CapsuleComponent.h"
#include "Data/ItemDefinitionDataAsset.h"
#include "Data/PieceDefinitionDataAsset.h"
#include "Data/VehicleBlockDefinitionDataAsset.h"
#include "Engine/Engine.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Vehicles/VehicleConstruct.h"
#include "Interfaces/Pilotable.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputActionValue.h"
#include "Net/UnrealNetwork.h"
#include "Exoneer.h"

APlayerSurvivalCharacter::APlayerSurvivalCharacter()
{
	PrimaryActorTick.bCanEverTick = true;

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("FPCamera"));
	Camera->SetupAttachment(GetCapsuleComponent());
	Camera->SetRelativeLocation(FVector(0.f, 0.f, 70.f));
	Camera->bUsePawnControlRotation = true;

	bUseControllerRotationYaw = true;
	if (UCharacterMovementComponent* CMC = GetCharacterMovement())
	{
		CMC->bOrientRotationToMovement = false;
		CMC->MaxWalkSpeed = WalkSpeed;
		CMC->JumpZVelocity = JumpHeight;
		CMC->AirControl = 0.5f;
		// Default push force launches light constructs across the map when
		// the engineer brushes against them; scale it to mass and calm it.
		CMC->PushForceFactor = 80000.f;
		CMC->bPushForceScaledToMass = true;
		CMC->bScalePushForceToVelocity = true;
	}

	Inventory     = CreateDefaultSubobject<UInventoryComponent>(TEXT("Inventory"));
	Inventory->bUseWeight = true;
	Inventory->MaxCapacity = 120.f;

	Survival      = CreateDefaultSubobject<USurvivalStatsComponent>(TEXT("Survival"));
	HealthC       = CreateDefaultSubobject<UHealthComponent>(TEXT("Health"));
	Interactor    = CreateDefaultSubobject<UInteractionComponent>(TEXT("Interactor"));
	MiningTool    = CreateDefaultSubobject<UMiningToolComponent>(TEXT("MiningTool"));
	BuildTool     = CreateDefaultSubobject<UBuildToolComponent>(TEXT("BuildTool"));
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
			PilotSendAccumulator = 0.f;
			FPilotInput Packet = PendingPilotInput;
			Packet.Brake = bBrakeHeld ? 1.f : 0.f;
			Packet.HeldFlags =
				(bHandbrakeHeld ? EPilotHeldFlags::Handbrake : 0)
				| (bTirePressureUpHeld ? EPilotHeldFlags::CtisUp : 0)
				| (bTirePressureDownHeld ? EPilotHeldFlags::CtisDown : 0);
			Packet.ModeToggleCount = ModeTogglePressCounter & 0x3;
			Server_SendPilotInput(PilotedConstruct, Packet);
			PendingPilotInput = FPilotInput();
		}
	}
}

void APlayerSurvivalCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(APlayerSurvivalCharacter, PilotedConstruct);
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
	Bind(IA_Jump,             ETriggerEvent::Triggered,  &APlayerSurvivalCharacter::Input_Jump);   // held = pilot up-thrust
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
	Bind(IA_TirePressureUp,   ETriggerEvent::Started,    &APlayerSurvivalCharacter::Input_TirePressureUpStart);
	Bind(IA_TirePressureUp,   ETriggerEvent::Completed,  &APlayerSurvivalCharacter::Input_TirePressureUpStop);
	Bind(IA_TirePressureDown, ETriggerEvent::Started,    &APlayerSurvivalCharacter::Input_TirePressureDownStart);
	Bind(IA_TirePressureDown, ETriggerEvent::Completed,  &APlayerSurvivalCharacter::Input_TirePressureDownStop);
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
		// Flight frame: X forward, Y right; Z comes from jump (thrust up).
		PendingPilotInput.Move.X = Axis.Y;
		PendingPilotInput.Move.Y = Axis.X;
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
		// Flight: rotate intent as (pitch, yaw, roll); camera stays seat-locked.
		PendingPilotInput.Rotate.X = -Axis.Y;
		PendingPilotInput.Rotate.Y = Axis.X;
		return;
	}
	// On foot, and Ground-mode piloting: the mouse is a free-look camera.
	AddControllerYawInput(Axis.X);
	AddControllerPitchInput(-Axis.Y);
}

void APlayerSurvivalCharacter::Input_Jump(const FInputActionValue&)
{
	if (PilotedConstruct)
	{
		if (PilotedConstruct->GetControlMode() == EPilotControlMode::Flight)
		{
			PendingPilotInput.Move.Z = 1.f;   // held = up-thrust
		}
		return;
	}
	Jump();
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
	// cycles the QuickBar selection and arms the build tool directly.
	if (QuickBar.Num() > 0 && BuildTool)
	{
		QuickBarIndex = (QuickBarIndex + 1) % QuickBar.Num();
		UPrimaryDataAsset* Selected = QuickBar[QuickBarIndex];
		FString Label = TEXT("(empty)");
		if (UPieceDefinitionDataAsset* Piece = Cast<UPieceDefinitionDataAsset>(Selected))
		{
			SetSelectedPiece(Piece);
			Label = Piece->DisplayName.ToString();
		}
		else if (UVehicleBlockDefinitionDataAsset* Block = Cast<UVehicleBlockDefinitionDataAsset>(Selected))
		{
			SetSelectedVehicleBlock(Block);
			Label = Block->DisplayName.ToString();
		}
		BuildTool->SetBuildModeEnabled(true);
		ToolMode = EPlayerToolMode::Build;
		(void)Label;   // The visor HUD renders the quick bar and selection now.
	}
	RequestOpenBuildMenuUI();
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
void APlayerSurvivalCharacter::Input_TirePressureUpStart(const FInputActionValue&)   { bTirePressureUpHeld = true; }
void APlayerSurvivalCharacter::Input_TirePressureUpStop(const FInputActionValue&)    { bTirePressureUpHeld = false; }
void APlayerSurvivalCharacter::Input_TirePressureDownStart(const FInputActionValue&) { bTirePressureDownHeld = true; }
void APlayerSurvivalCharacter::Input_TirePressureDownStop(const FInputActionValue&)  { bTirePressureDownHeld = false; }

void APlayerSurvivalCharacter::Input_ToggleControlMode(const FInputActionValue&)
{
	if (PilotedConstruct)
	{
		// Rolling counter: the press latches even if it lands between sends.
		ModeTogglePressCounter = (ModeTogglePressCounter + 1) & 0x3;
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
