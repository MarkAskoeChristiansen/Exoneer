// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Interfaces/Damageable.h"
#include "Interfaces/InventoryOwner.h"
#include "ExoneerTypes.h"
#include "Vehicles/PilotInput.h"
#include "PlayerSurvivalCharacter.generated.h"

class UCameraComponent;
class USpringArmComponent;
class UPrimaryDataAsset;
class UInputAction;
class UInputMappingContext;
class UInventoryComponent;
class USurvivalStatsComponent;
class UHealthComponent;
class UInteractionComponent;
class UMiningToolComponent;
class UBuildToolComponent;
class UPieceDefinitionDataAsset;
class UVehicleBlockDefinitionDataAsset;
class AVehicleConstruct;

UENUM(BlueprintType)
enum class EPlayerToolMode : uint8
{
	None,
	Mining,
	Build,
	Weld
};

/**
 * The first-person player pawn.
 *
 * Owns:
 *  - InventoryComponent (suit cargo, weight-based)
 *  - SurvivalStatsComponent (oxygen, suit power, nutrition, temperature)
 *  - HealthComponent (HP)
 *  - InteractionComponent (focus + interact)
 *  - MiningToolComponent
 *  - BuildToolComponent
 *
 * Enhanced Input is read from the bound IMC on possession.
 *
 * Piloting: the server seats this pawn on an AVehicleConstruct via
 * SetPilotedConstruct (called from the construct's EnterPilot/ExitPilot).
 * While seated, move/look input is suppressed locally and forwarded to the
 * construct through Server_SendPilotInput at ~PilotInputSendHz, because the
 * construct itself is not connection-owned and cannot receive client RPCs.
 */
UCLASS(BlueprintType, Blueprintable)
class EXONEER_API APlayerSurvivalCharacter : public ACharacter, public IDamageable, public IInventoryOwner
{
	GENERATED_BODY()

public:
	APlayerSurvivalCharacter();

	// --- Components ---
	UPROPERTY(VisibleAnywhere) UCameraComponent* Camera = nullptr;
	UPROPERTY(VisibleAnywhere) UInventoryComponent* Inventory = nullptr;
	UPROPERTY(VisibleAnywhere) USurvivalStatsComponent* Survival = nullptr;
	UPROPERTY(VisibleAnywhere) UHealthComponent* HealthC = nullptr;
	UPROPERTY(VisibleAnywhere) UInteractionComponent* Interactor = nullptr;
	UPROPERTY(VisibleAnywhere) UMiningToolComponent* MiningTool = nullptr;
	UPROPERTY(VisibleAnywhere) UBuildToolComponent* BuildTool = nullptr;

	// --- Enhanced Input bindings (assigned on Blueprint child) ---
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input") UInputMappingContext* DefaultMappingContext = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input") UInputAction* IA_Move = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input") UInputAction* IA_Look = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input") UInputAction* IA_Jump = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input") UInputAction* IA_Sprint = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input") UInputAction* IA_Crouch = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input") UInputAction* IA_Interact = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input") UInputAction* IA_PrimaryAction = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input") UInputAction* IA_SecondaryAction = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input") UInputAction* IA_OpenInventory = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input") UInputAction* IA_OpenBuildMenu = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input") UInputAction* IA_RotateBlock = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input") UInputAction* IA_ConfirmPlace = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input") UInputAction* IA_CancelPlace = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input") UInputAction* IA_ToggleTool = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input") UInputAction* IA_EnterExitCockpit = nullptr;
	// Piloting extras (names must match the bootstrap-generated IA assets exactly).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input") UInputAction* IA_Brake = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input") UInputAction* IA_Handbrake = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input") UInputAction* IA_ToggleControlMode = nullptr;

	// --- Tunables ---
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement") float WalkSpeed = 450.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement") float SprintSpeed = 800.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement") float JumpHeight = 460.f;

	/** How often accumulated pilot input is sent to the server while seated. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Piloting") float PilotInputSendHz = 20.f;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Tool") EPlayerToolMode ToolMode = EPlayerToolMode::Mining;

	/** Construct this pawn currently pilots; set by the SERVER via SetPilotedConstruct. */
	UPROPERTY(Replicated, VisibleInstanceOnly, BlueprintReadOnly, Category = "Piloting")
	TObjectPtr<AVehicleConstruct> PilotedConstruct;

	/** SERVER. Called by AVehicleConstruct when this pawn is seated/released. */
	void SetPilotedConstruct(AVehicleConstruct* Construct);

	UFUNCTION(BlueprintPure, Category = "Piloting") bool IsPiloting() const { return PilotedConstruct != nullptr; }

	UFUNCTION(BlueprintPure, Category = "Debug") int32 GetQuickBarIndex() const { return QuickBarIndex; }

	// --- BP-callable helpers ---
	UFUNCTION(BlueprintCallable) void CycleToolMode();
	UFUNCTION(BlueprintCallable) void SetSelectedPiece(UPieceDefinitionDataAsset* Piece);
	UFUNCTION(BlueprintCallable) void SetSelectedVehicleBlock(UVehicleBlockDefinitionDataAsset* Block);
	UFUNCTION(BlueprintImplementableEvent) void RequestOpenInventoryUI();
	UFUNCTION(BlueprintImplementableEvent) void RequestOpenBuildMenuUI();

	// --- Prototype debug affordances (until the diegetic UI stack exists) ---

	/** Buildables the build-menu key cycles through (pieces or vehicle blocks). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	TArray<TObjectPtr<UPrimaryDataAsset>> QuickBar;

	/** Granted once on the server at spawn so the build loop can bootstrap. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	TArray<FInventoryEntry> StarterItems;

	// IDamageable
	virtual float ApplyExoneerDamage_Implementation(float Amount, EExoneerDamageType Type, AActor* Instigator) override;
	virtual float GetCurrentHealth_Implementation() const override;
	virtual float GetMaxHealth_Implementation() const override;

	// IInventoryOwner
	virtual UInventoryComponent* GetInventory_Implementation() const override { return Inventory; }

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	// Input handlers.
	void Input_Move(const struct FInputActionValue& Value);
	void Input_Look(const struct FInputActionValue& Value);
	void Input_Jump(const struct FInputActionValue& Value);
	void Input_SprintStart(const struct FInputActionValue& Value);
	void Input_SprintStop(const struct FInputActionValue& Value);
	void Input_CrouchToggle(const struct FInputActionValue& Value);
	void Input_Interact(const struct FInputActionValue& Value);
	void Input_PrimaryStart(const struct FInputActionValue& Value);
	void Input_PrimaryStop(const struct FInputActionValue& Value);
	void Input_SecondaryStart(const struct FInputActionValue& Value);
	void Input_SecondaryStop(const struct FInputActionValue& Value);
	void Input_OpenInventory(const struct FInputActionValue& Value);
	void Input_OpenBuildMenu(const struct FInputActionValue& Value);
	void Input_RotateBlock(const struct FInputActionValue& Value);
	void Input_ConfirmPlace(const struct FInputActionValue& Value);
	void Input_CancelPlace(const struct FInputActionValue& Value);
	void Input_ToggleTool(const struct FInputActionValue& Value);
	void Input_EnterExitCockpit(const struct FInputActionValue& Value);
	void Input_BrakeStart(const struct FInputActionValue& Value);
	void Input_BrakeStop(const struct FInputActionValue& Value);
	void Input_HandbrakeStart(const struct FInputActionValue& Value);
	void Input_HandbrakeStop(const struct FInputActionValue& Value);
	void Input_ToggleControlMode(const struct FInputActionValue& Value);

	// --- Piloting (client intent -> server; the construct is not connection-owned) ---

	/**
	 * Pilot intent packet, batched to ~PilotInputSendHz in Tick. Unreliable:
	 * the next send supersedes a lost one; held states ride as flags and
	 * discrete presses as a rolling counter, so neither is lost to timing.
	 */
	UFUNCTION(Server, Unreliable, WithValidation)
	void Server_SendPilotInput(AVehicleConstruct* Construct, FPilotInput Input);

	/** Leave the cockpit. Reliable: a lost exit would strand the pilot. */
	UFUNCTION(Server, Reliable, WithValidation)
	void Server_ExitPilot();

	/** Axis samples for the current send window (zeroed after each send). */
	FPilotInput PendingPilotInput;
	float PilotSendAccumulator = 0.f;

	/** Held key states, sampled into the packet at send time (never zeroed by sends). */
	bool bBrakeHeld = false;
	bool bHandbrakeHeld = false;

	/** Rolling 2-bit counter of control-mode toggle presses. */
	uint8 ModeTogglePressCounter = 0;

	/** Current QuickBar selection (debug build-menu cycling). */
	int32 QuickBarIndex = INDEX_NONE;
};
