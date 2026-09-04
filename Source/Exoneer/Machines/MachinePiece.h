// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Building/BasePiece.h"
#include "ExoneerTypes.h"
#include "MachinePiece.generated.h"

class UPowerComponent;
class UInventoryComponent;
class UConveyorComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnMachineStateChanged, EMachineState, NewState);

/**
 * Base class for deployable powered machines (refinery, fabricator, oxygen
 * generator, battery, solar panel). A machine is a base piece (MountTag
 * Exoneer.Mount.Deployable) with power, an internal inventory, a conveyor
 * link, and a replicated machine state driving UI/VFX.
 *
 * Machines are inert until construction completes; ABaseStructure registers
 * the PowerComponent on completion and unregisters on removal.
 */
UCLASS(BlueprintType, Blueprintable)
class EXONEER_API AMachinePiece : public ABasePiece
{
	GENERATED_BODY()

public:
	AMachinePiece();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Machine")
	TObjectPtr<UPowerComponent> Power;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Machine")
	TObjectPtr<UInventoryComponent> Inventory;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Machine")
	TObjectPtr<UConveyorComponent> Conveyor;

	UPROPERTY(ReplicatedUsing = OnRep_MachineState, VisibleAnywhere, BlueprintReadOnly, Category = "Machine")
	EMachineState MachineState = EMachineState::Idle;

	UPROPERTY(BlueprintAssignable) FOnMachineStateChanged OnMachineStateChanged;

	/** Client-side UI entry point; fired from OnInteractLocal. */
	UFUNCTION(BlueprintNativeEvent, Category = "Machine")
	bool OpenMachineUI(APawn* User);
	virtual bool OpenMachineUI_Implementation(APawn* User) { return false; }

	/** SERVER. Recompute and replicate MachineState (called by subclasses/tick). */
	void UpdateMachineState(EMachineState NewState);

	/** Joules this pack can still hold: rated storage less capacity fade. */
	UFUNCTION(BlueprintPure, Category = "Machine")
	float GetEffectiveEnergyStorageJ() const;

	/**
	 * SERVER. Age the pack by ThroughputJ joules moved in or out at AmbientC,
	 * called by the structure's power network once per sim step. The published
	 * reading moves in ExoneerMaintenance::CapacityFadeDeadband steps; when it
	 * does, the derated storage capacity is re-applied and the stored energy
	 * clamped into it.
	 */
	void ApplyEnergyThroughput(float ThroughputJ, float AmbientC);

	/** Applies Def-driven stats (power draw/output derated by fade, inventory capacity). */
	virtual void ApplyDefinitionStats();

	virtual void ResetConditionToNominal() override;

	// IInteractable: server side is a no-op for plain machines; UI opens locally.
	virtual bool OnInteract_Implementation(AActor* Interactor) override;
	virtual void OnInteractLocal_Implementation(AActor* Interactor) override;
	virtual FGameplayTagContainer GetInteractionTags_Implementation() const override;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

protected:
	virtual void BeginPlay() override;

	UFUNCTION() void OnRep_MachineState();

	/** SERVER, transient. Fade banked against the FadeDeadband. */
	float PendingCapacityFade = 0.f;
};
