// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Interfaces/Damageable.h"
#include "Interfaces/InventoryOwner.h"
#include "PlayerSurvivalCharacter.generated.h"

class UCameraComponent;
class USpringArmComponent;
class UInputAction;
class UInputMappingContext;
class UInventoryComponent;
class USurvivalStatsComponent;
class UHealthComponent;
class UInteractionComponent;
class UMiningToolComponent;
class UBuildToolComponent;
class UBlockDefinitionDataAsset;

UENUM(BlueprintType)
enum class EPlayerToolMode : uint8
{
	None,
	Mining,
	Build,
	Repair
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

	// --- Tunables ---
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement") float WalkSpeed = 450.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement") float SprintSpeed = 800.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement") float JumpHeight = 460.f;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Tool") EPlayerToolMode ToolMode = EPlayerToolMode::Mining;

	// --- BP-callable helpers ---
	UFUNCTION(BlueprintCallable) void CycleToolMode();
	UFUNCTION(BlueprintCallable) void SetSelectedBuildBlock(UBlockDefinitionDataAsset* Block);
	UFUNCTION(BlueprintImplementableEvent) void RequestOpenInventoryUI();
	UFUNCTION(BlueprintImplementableEvent) void RequestOpenBuildMenuUI();
	UFUNCTION(BlueprintImplementableEvent) void RequestEnterExitCockpit();

	// IDamageable
	virtual float ApplyExoneerDamage_Implementation(float Amount, EExoneerDamageType Type, AActor* Instigator) override;
	virtual float GetCurrentHealth_Implementation() const override;
	virtual float GetMaxHealth_Implementation() const override;

	// IInventoryOwner
	virtual UInventoryComponent* GetInventory_Implementation() const override { return Inventory; }

	virtual void BeginPlay() override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

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
	void Input_OpenInventory(const struct FInputActionValue& Value);
	void Input_OpenBuildMenu(const struct FInputActionValue& Value);
	void Input_RotateBlock(const struct FInputActionValue& Value);
	void Input_ConfirmPlace(const struct FInputActionValue& Value);
	void Input_CancelPlace(const struct FInputActionValue& Value);
	void Input_ToggleTool(const struct FInputActionValue& Value);
	void Input_EnterExitCockpit(const struct FInputActionValue& Value);
};
