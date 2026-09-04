// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Interfaces/Damageable.h"
#include "Interfaces/InventoryOwner.h"
#include "ExoneerTypes.h"
#include "Vehicles/PilotInput.h"
#include "CableComponent.h"
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
class AUmbilicalPortPiece;

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
	/**
	 * The visor: the ONLY view on foot, and the origin of every tool and
	 * interaction ray (see GetActorEyesViewPoint). Active whenever the
	 * chase camera is not.
	 */
	UPROPERTY(VisibleAnywhere) UCameraComponent* Camera = nullptr;

	/**
	 * Chase boom, used only while seated in a cockpit. It hangs off the
	 * capsule, and the capsule is welded to the cockpit block, so the boom
	 * rides the construct's rigid body: it inherits heading on the ground and
	 * full attitude (roll included) in flight without any smoothing that would
	 * hide what the suspension is doing.
	 */
	UPROPERTY(VisibleAnywhere) USpringArmComponent* ChaseArm = nullptr;
	UPROPERTY(VisibleAnywhere) UCameraComponent* ChaseCamera = nullptr;
	UPROPERTY(VisibleAnywhere) UInventoryComponent* Inventory = nullptr;
	UPROPERTY(VisibleAnywhere) USurvivalStatsComponent* Survival = nullptr;
	UPROPERTY(VisibleAnywhere) UHealthComponent* HealthC = nullptr;
	UPROPERTY(VisibleAnywhere) UInteractionComponent* Interactor = nullptr;
	UPROPERTY(VisibleAnywhere) UMiningToolComponent* MiningTool = nullptr;
	UPROPERTY(VisibleAnywhere) UBuildToolComponent* BuildTool = nullptr;

	/** Visual umbilical; start on the capsule, end on the connected port. */
	UPROPERTY(VisibleAnywhere) TObjectPtr<UCableComponent> UmbilicalCable = nullptr;

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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input") UInputAction* IA_CycleVehicleBlock = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input") UInputAction* IA_RotateBlock = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input") UInputAction* IA_ConfirmPlace = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input") UInputAction* IA_CancelPlace = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input") UInputAction* IA_ToggleTool = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input") UInputAction* IA_EnterExitCockpit = nullptr;
	// Piloting extras (names must match the bootstrap-generated IA assets exactly).
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input") UInputAction* IA_Brake = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input") UInputAction* IA_Handbrake = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input") UInputAction* IA_ToggleControlMode = nullptr;
	/** Mouse wheel: chase-boom length while seated. Axis1D, signed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Input") UInputAction* IA_CameraZoom = nullptr;

	// --- Tunables ---
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement") float WalkSpeed = 450.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement") float SprintSpeed = 800.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Movement") float JumpHeight = 460.f;

	/** How often accumulated pilot input is sent to the server while seated. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Piloting") float PilotInputSendHz = 20.f;

	/**
	 * Mouse travel per second that asks for a FULL-deflection attitude rate
	 * while flying. The flight stick is a rate command, so the raw per-frame
	 * delta has to become counts-per-second first: without this the mouse was
	 * a digital switch, any motion above one unit per frame demanding full
	 * rated torque, and only the last frame of each send window was even
	 * transmitted. That alternation was the 20 Hz judder in the gyro.
	 *
	 * A sensitivity number, judged by feel, not a derivation. At 1200 against a
	 * 30 deg/s rate limit an ordinary 200 counts/s move asked for 5 deg/s while
	 * A/D reached the ceiling, so pitch and yaw felt dead against the keyboard.
	 * 400 overcorrected: it put about 1.5 times the gain of the build the owner
	 * called almost uncontrollable onto the same stick. 600 gives an ordinary
	 * 200 counts/s move a third of full rate, one turn of the wrist reaches the
	 * ceiling, and it sits BELOW the gain of the build that was complained
	 * about rather than above it. The same accumulator drives a roughly
	 * degree-per-count camera on foot, so the two should not disagree wildly.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Piloting") float PilotLookCountsPerSecFullRate = 600.f;

	/**
	 * Chase-boom limits, in cm, sized on the 6x6 test rover: a 3.00 m x 1.00 m
	 * hull on a 1.50 m track, about 1.15 m tall over the tires, body diagonal
	 * ~3.4 m, cockpit at the FRONT of the hull. At the engine's 90 deg
	 * horizontal FOV an object subtends half the screen width at a range equal
	 * to its own size, so 3.4 m frames the rover edge to edge. 2.50 m is the
	 * shortest boom that still clears the 3.00 m of hull it points down;
	 * 4.50 m gives the vehicle room in frame; 12.00 m shows the rover plus
	 * roughly 8 m of ground ahead, which is what reading a slab edge before
	 * you reach it needs.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera") float ChaseArmMinCm = 250.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera") float ChaseArmMaxCm = 1200.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera") float ChaseArmDefaultCm = 450.f;

	/** One wheel notch, cm. 13 notches walk the whole range end to end. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera") float ChaseZoomStepCm = 75.f;

	/** Boom length interpolation rate, 1/s. The wheel sets a target, never the length. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera") float ChaseZoomInterpSpeed = 10.f;

	/** Boom down-angle, deg, and camera rise above the boom, cm. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera") float ChaseArmPitchDeg = -12.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera") float ChaseCameraRiseCm = 90.f;

	/**
	 * Boom position lag, 1/s. ZERO ON PURPOSE: a lagged boom smooths the
	 * suspension out of the picture, and the suspension is a physics bug being
	 * hunted, not something to hide. Raise it (5-10 is the usual range) only
	 * once the wheel reports are closed.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera") float ChaseCameraLagSpeed = 0.f;

	UPROPERTY(VisibleInstanceOnly, BlueprintReadOnly, Category = "Tool") EPlayerToolMode ToolMode = EPlayerToolMode::Mining;

	/** Construct this pawn currently pilots; set by the SERVER via SetPilotedConstruct. */
	UPROPERTY(ReplicatedUsing = OnRep_PilotedConstruct, VisibleInstanceOnly, BlueprintReadOnly, Category = "Piloting")
	TObjectPtr<AVehicleConstruct> PilotedConstruct;

	UFUNCTION() void OnRep_PilotedConstruct();

	/**
	 * Port this pawn is plugged into. Server writes; clients update the cable
	 * from the RepNotify. Null when unplugged.
	 */
	UPROPERTY(ReplicatedUsing = OnRep_UmbilicalSource, VisibleInstanceOnly, BlueprintReadOnly, Category = "Suit")
	TObjectPtr<AUmbilicalPortPiece> UmbilicalSource;

	UFUNCTION() void OnRep_UmbilicalSource();

	/**
	 * World time the last range-break unplug happened. HUD prints
	 * "umbilical detached" for a few seconds after. -1 = never.
	 */
	UPROPERTY(Replicated, VisibleInstanceOnly, BlueprintReadOnly, Category = "Suit")
	float UmbilicalDetachedAt = -1.f;

	/** SERVER. Plug or unplug. bRangeBreak stamps UmbilicalDetachedAt. */
	void SetUmbilicalSource(AUmbilicalPortPiece* Port, bool bRangeBreak);

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

	/**
	 * Cycle the quick bar within ONE kind: B walks the base pieces, N the
	 * vehicle blocks. One list, two cursors - the list grew past the point
	 * where cycling through everything to reach a wheel was usable.
	 */
	UFUNCTION(BlueprintCallable) void CycleQuickBar(bool bVehicleBlocks);

	/** Buildables the quick-bar keys cycle through (pieces on B, vehicle blocks on N). */
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
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void Landed(const FHitResult& Hit) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/**
	 * Aim origin for EVERY tool and interaction ray: the visor camera's
	 * position with the current view rotation. Stated here once so no call
	 * site has to guess which camera component it found - with a chase boom on
	 * the same pawn, FindComponentByClass<UCameraComponent> returns an
	 * arbitrary one and the placement ray could start metres behind the
	 * engineer. Reach validation on the server runs through this same point.
	 */
	virtual void GetActorEyesViewPoint(FVector& OutLocation, FRotator& OutRotation) const override;

protected:
	// Input handlers.
	void Input_Move(const struct FInputActionValue& Value);
	void Input_Look(const struct FInputActionValue& Value);
	void Input_Jump(const struct FInputActionValue& Value);
	void Input_JumpStop(const struct FInputActionValue& Value);
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
	void Input_CycleVehicleBlock(const struct FInputActionValue& Value);
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
	void Input_CameraZoom(const struct FInputActionValue& Value);

	/**
	 * Point the view at whatever the pawn is doing now: chase boom while
	 * seated, visor on foot. Called from SetPilotedConstruct (authority, which
	 * covers a listen host's own pawn - an OnRep never fires there) and from
	 * OnRep_PilotedConstruct (remote clients).
	 */
	void UpdateSeatView();

	/** Visor machine UI: 1-8 enqueue focused station recipes. */
	void EnqueueFocusedRecipe(int32 Index);
	/** Wrist projects: J/K/L accept or abandon catalog slots. */
	void ToggleProjectByIndex(int32 Index);

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

	UFUNCTION(Server, Reliable, WithValidation)
	void Server_ToggleProject(int32 Index);

	/** Client landing impact; the server spends the seal. Speed is m/s. */
	UFUNCTION(Server, Reliable, WithValidation)
	void Server_ReportLanding(float ImpactSpeedMps);

	/** Axis samples for the current send window (zeroed after each send). */
	FPilotInput PendingPilotInput;
	float PilotSendAccumulator = 0.f;

	/** Held key states, sampled into the packet at send time (never zeroed by sends). */
	bool bBrakeHeld = false;
	bool bHandbrakeHeld = false;
	bool bLiftHeld = false;

	/**
	 * Mouse counts accumulated over the whole send window. Overwriting this
	 * per frame (the old behaviour) threw away every frame but the last and
	 * sent zero whenever the last frame happened to be still.
	 */
	FVector2D PendingLookCounts = FVector2D::ZeroVector;

	/** Rolling 2-bit counter of control-mode toggle presses. */
	uint8 ModeTogglePressCounter = 0;

	/** Currently armed QuickBar entry (the one the visor highlights). */
	int32 QuickBarIndex = INDEX_NONE;

	/** Per-kind cursors, so B and N each keep their own place in the one list. */
	int32 PieceQuickBarIndex = INDEX_NONE;
	int32 BlockQuickBarIndex = INDEX_NONE;

	/** Boom length the wheel is asking for, cm. Survives the whole session. */
	float DesiredChaseArmCm = 450.f;

	/**
	 * Control rotation at the moment of seating. Free-look while driving is
	 * the DIFFERENCE from this, applied to the boom in seat space, so the view
	 * turns with the vehicle and the mouse offset rides on top of it.
	 */
	FRotator SeatLookReference = FRotator::ZeroRotator;

	/** True while the chase boom owns the view, so the swap runs only on change. */
	bool bSeatViewActive = false;

	void ApplyLandingSealWear(float ImpactSpeedMps);
	void UpdateUmbilicalCable();
};
