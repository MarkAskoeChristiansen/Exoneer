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
#include "GameFramework/CharacterMovementComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputActionValue.h"
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

void APlayerSurvivalCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	UEnhancedInputComponent* EIC = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!EIC) return;

	auto Bind = [&](UInputAction* IA, ETriggerEvent E, auto Fn)
	{
		if (IA) EIC->BindAction(IA, E, this, Fn);
	};

	Bind(IA_Move,             ETriggerEvent::Triggered,  &APlayerSurvivalCharacter::Input_Move);
	Bind(IA_Look,             ETriggerEvent::Triggered,  &APlayerSurvivalCharacter::Input_Look);
	Bind(IA_Jump,             ETriggerEvent::Triggered,  &APlayerSurvivalCharacter::Input_Jump);
	Bind(IA_Sprint,           ETriggerEvent::Started,    &APlayerSurvivalCharacter::Input_SprintStart);
	Bind(IA_Sprint,           ETriggerEvent::Completed,  &APlayerSurvivalCharacter::Input_SprintStop);
	Bind(IA_Crouch,           ETriggerEvent::Triggered,  &APlayerSurvivalCharacter::Input_CrouchToggle);
	Bind(IA_Interact,         ETriggerEvent::Triggered,  &APlayerSurvivalCharacter::Input_Interact);
	Bind(IA_PrimaryAction,    ETriggerEvent::Started,    &APlayerSurvivalCharacter::Input_PrimaryStart);
	Bind(IA_PrimaryAction,    ETriggerEvent::Completed,  &APlayerSurvivalCharacter::Input_PrimaryStop);
	Bind(IA_SecondaryAction,  ETriggerEvent::Triggered,  &APlayerSurvivalCharacter::Input_SecondaryStart);
	Bind(IA_OpenInventory,    ETriggerEvent::Triggered,  &APlayerSurvivalCharacter::Input_OpenInventory);
	Bind(IA_OpenBuildMenu,    ETriggerEvent::Triggered,  &APlayerSurvivalCharacter::Input_OpenBuildMenu);
	Bind(IA_RotateBlock,      ETriggerEvent::Triggered,  &APlayerSurvivalCharacter::Input_RotateBlock);
	Bind(IA_ConfirmPlace,     ETriggerEvent::Triggered,  &APlayerSurvivalCharacter::Input_ConfirmPlace);
	Bind(IA_CancelPlace,      ETriggerEvent::Triggered,  &APlayerSurvivalCharacter::Input_CancelPlace);
	Bind(IA_ToggleTool,       ETriggerEvent::Triggered,  &APlayerSurvivalCharacter::Input_ToggleTool);
	Bind(IA_EnterExitCockpit, ETriggerEvent::Triggered,  &APlayerSurvivalCharacter::Input_EnterExitCockpit);
}

void APlayerSurvivalCharacter::Input_Move(const FInputActionValue& Value)
{
	const FVector2D Axis = Value.Get<FVector2D>();
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
	AddControllerYawInput(Axis.X);
	AddControllerPitchInput(-Axis.Y);
}

void APlayerSurvivalCharacter::Input_Jump(const FInputActionValue&)
{
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
	if (bIsCrouched) UnCrouch(); else Crouch();
}

void APlayerSurvivalCharacter::Input_Interact(const FInputActionValue&)
{
	if (Interactor) Interactor->TryInteract();
}

void APlayerSurvivalCharacter::Input_PrimaryStart(const FInputActionValue&)
{
	switch (ToolMode)
	{
	case EPlayerToolMode::Mining: if (MiningTool) MiningTool->SetActive(true); break;
	case EPlayerToolMode::Build:  if (BuildTool)  BuildTool->TryConfirmPlacement(); break;
	case EPlayerToolMode::Repair: if (BuildTool)  BuildTool->TryRepairTargetedBlock(20.f); break;
	default: break;
	}
}

void APlayerSurvivalCharacter::Input_PrimaryStop(const FInputActionValue&)
{
	if (MiningTool) MiningTool->SetActive(false);
}

void APlayerSurvivalCharacter::Input_SecondaryStart(const FInputActionValue&)
{
	if (ToolMode == EPlayerToolMode::Build && BuildTool)
	{
		BuildTool->TryRemoveTargetedBlock();
	}
}

void APlayerSurvivalCharacter::Input_OpenInventory(const FInputActionValue&)
{
	RequestOpenInventoryUI();
}

void APlayerSurvivalCharacter::Input_OpenBuildMenu(const FInputActionValue&)
{
	RequestOpenBuildMenuUI();
}

void APlayerSurvivalCharacter::Input_RotateBlock(const FInputActionValue&)
{
	if (BuildTool) BuildTool->RotateBlock(1);
}

void APlayerSurvivalCharacter::Input_ConfirmPlace(const FInputActionValue&)
{
	if (BuildTool) BuildTool->TryConfirmPlacement();
}

void APlayerSurvivalCharacter::Input_CancelPlace(const FInputActionValue&)
{
	if (BuildTool) BuildTool->SetBuildModeEnabled(false);
	ToolMode = EPlayerToolMode::Mining;
}

void APlayerSurvivalCharacter::Input_ToggleTool(const FInputActionValue&)
{
	CycleToolMode();
}

void APlayerSurvivalCharacter::Input_EnterExitCockpit(const FInputActionValue&)
{
	RequestEnterExitCockpit();
}

void APlayerSurvivalCharacter::CycleToolMode()
{
	switch (ToolMode)
	{
	case EPlayerToolMode::None:   ToolMode = EPlayerToolMode::Mining; break;
	case EPlayerToolMode::Mining: ToolMode = EPlayerToolMode::Build;  break;
	case EPlayerToolMode::Build:  ToolMode = EPlayerToolMode::Repair; break;
	case EPlayerToolMode::Repair: ToolMode = EPlayerToolMode::Mining; break;
	}
	if (BuildTool) BuildTool->SetBuildModeEnabled(ToolMode == EPlayerToolMode::Build);
	if (MiningTool) MiningTool->SetActive(false);
}

void APlayerSurvivalCharacter::SetSelectedBuildBlock(UBlockDefinitionDataAsset* Block)
{
	if (BuildTool) BuildTool->SetSelectedBlock(Block);
}

float APlayerSurvivalCharacter::ApplyExoneerDamage_Implementation(float Amount, EExoneerDamageType Type, AActor* Instigator)
{
	return HealthC ? HealthC->ApplyDamage(Amount, Type, Instigator) : 0.f;
}

float APlayerSurvivalCharacter::GetCurrentHealth_Implementation() const
{
	return HealthC ? HealthC->Health : 0.f;
}

float APlayerSurvivalCharacter::GetMaxHealth_Implementation() const
{
	return HealthC ? HealthC->MaxHealth : 100.f;
}
