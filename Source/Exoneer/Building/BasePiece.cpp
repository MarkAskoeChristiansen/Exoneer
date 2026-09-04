// Copyright Exoneer contributors.
#include "Building/BasePiece.h"
#include "Building/BaseStructure.h"
#include "Components/ConstructionComponent.h"
#include "Data/PieceDefinitionDataAsset.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Net/UnrealNetwork.h"
#include "Maintenance/ExoneerMaintenance.h"
#include "Vehicles/ExoneerVehicleUnits.h"   // ECC_WheelProbe
#include "Components/InventoryComponent.h"
#include "Data/ItemDefinitionDataAsset.h"
#include "Engine/AssetManager.h"

ABasePiece::ABasePiece()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;

	// The root stays at the mount point; the mesh is a CHILD so alignment
	// offsets (bottom-aligning centered placeholder pivots) move only the
	// visual. Offsetting a root component would teleport the whole actor.
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Root);
	Mesh->SetMobility(EComponentMobility::Movable);
	Mesh->SetCollisionProfileName(TEXT("BlockAll"));

	Construction = CreateDefaultSubobject<UConstructionComponent>(TEXT("Construction"));
}

void ABasePiece::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ABasePiece, Def);
	DOREPLIFETIME(ABasePiece, OwningStructure);
	DOREPLIFETIME(ABasePiece, ParentPiece);
	DOREPLIFETIME(ABasePiece, ParentSocket);
	DOREPLIFETIME(ABasePiece, Health);
	DOREPLIFETIME(ABasePiece, SupportValue);
	DOREPLIFETIME(ABasePiece, Condition);
	DOREPLIFETIME(ABasePiece, LastLoadN);
}

void ABasePiece::BeginPlay()
{
	Super::BeginPlay();

	// Both sides react to phase changes: the server drives completion
	// bookkeeping, every machine refreshes mesh/collision. Bind before the
	// first refresh so no transition is missed.
	if (Construction)
	{
		Construction->OnPhaseChanged.AddDynamic(this, &ABasePiece::HandlePhaseChanged);
	}
	RefreshVisualState();
}

/**
 * SERVER. The spawning side (ABaseStructure) uses SpawnActorDeferred, calls
 * InitializeGhost, then FinishSpawning, so Def/OwningStructure/ParentPiece are
 * set before BeginPlay and before the actor's first replication update.
 */
void ABasePiece::InitializeGhost(ABaseStructure* Structure, UPieceDefinitionDataAsset* InDef, ABasePiece* InParent, FName InParentSocket)
{
	OwningStructure = Structure;
	Def = InDef;
	ParentPiece = InParent;
	ParentSocket = InParentSocket;
	Health = Def ? Def->MaxHealth : 1.f;
	SupportValue = 0;

	if (Construction && Def)
	{
		Construction->InitializeStages(Def->Stages);
	}
	RefreshVisualState();
}

void ABasePiece::OnRep_Def()
{
	// The definition can land after the first phase notification on a joining
	// client; refresh once it is known so the right mesh appears.
	RefreshVisualState();
}

void ABasePiece::OnRep_Health()
{
	// Health replicates for HUD bars and damage-state visuals; the HUD reads
	// the value directly and Blueprints can poll it, so no push logic here.
}

void ABasePiece::RefreshVisualState()
{
	if (!Mesh)
	{
		return;
	}

	const EConstructionPhase Phase = Construction ? Construction->GetPhase() : EConstructionPhase::Complete;

	// Resolve the display mesh. Ghosts prefer the dedicated ghost mesh and
	// fall back to the final one; the translucent ghost look and the weld
	// progress scalar are material concerns driven from Blueprint through the
	// phase delegates and OnConstructionCompletedBP, never hardcoded here.
	UStaticMesh* DisplayMesh = nullptr;
	if (Def)
	{
		if (Phase == EConstructionPhase::Ghost && !Def->GhostMesh.IsNull())
		{
			DisplayMesh = Def->GhostMesh.LoadSynchronous();
		}
		if (!DisplayMesh && !Def->Mesh.IsNull())
		{
			DisplayMesh = Def->Mesh.LoadSynchronous();
		}
	}
	if (DisplayMesh && Mesh->GetStaticMesh() != DisplayMesh)
	{
		Mesh->SetStaticMesh(DisplayMesh);
	}

	// Bottom-align: the piece origin is its mount point (socket / ground hit),
	// but placeholder meshes have centered pivots - without this shift a
	// grounded foundation sits half-buried in the floor.
	if (UStaticMesh* Current = Mesh->GetStaticMesh())
	{
		const FBox Bounds = Current->GetBoundingBox();
		Mesh->SetRelativeLocation(FVector(0.f, 0.f, -Bounds.Min.Z));
	}

	// Collision: ghosts stay visible to build/weld traces (QueryOnly) but let
	// pawns walk through; anything past ghost is solid world geometry.
	Mesh->SetCollisionProfileName(TEXT("BlockAll"));
	if (Phase == EConstructionPhase::Ghost)
	{
		Mesh->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Mesh->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
		// A ghost is a planning marker, not ground. The wheel probe channel
		// defaults to Block, so without this an unbuilt deck caught a wheel:
		// the suspension found a plane it could stand on, and the piece was
		// asked to carry a load it has not been welded to carry.
		Mesh->SetCollisionResponseToChannel(ECC_WheelProbe, ECR_Ignore);
	}
	else
	{
		Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	}

	// Until real ghost materials are authored, unfinished pieces render as
	// wireframe so "placed" and "welded" are visually unmistakable (players
	// were walking through ghosts that looked like solid built blocks).
	static UMaterialInterface* GhostMaterial =
		LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineDebugMaterials/WireframeMaterial.WireframeMaterial"));
	if (GhostMaterial)
	{
		if (Phase != EConstructionPhase::Complete)
		{
			for (int32 i = 0; i < Mesh->GetNumMaterials(); ++i)
			{
				Mesh->SetMaterial(i, GhostMaterial);
			}
		}
		else if (Mesh->GetMaterial(0) == GhostMaterial)
		{
			for (int32 i = 0; i < Mesh->GetNumMaterials(); ++i)
			{
				Mesh->SetMaterial(i, nullptr);   // Back to the mesh's own material.
			}
		}
	}
}

void ABasePiece::HandlePhaseChanged(EConstructionPhase NewPhase)
{
	RefreshVisualState();

	if (NewPhase == EConstructionPhase::Complete)
	{
		// Cosmetic completion hook. The delegate fires on the server from
		// SetPhase and on clients from the phase RepNotify, so FX run everywhere.
		OnConstructionCompletedBP();

		if (HasAuthority() && OwningStructure)
		{
			OwningStructure->NotifyPieceCompleted(this);
		}
	}
	else if (HasAuthority() && OwningStructure)
	{
		// Deconstruction can demote a Complete piece back to UnderConstruction;
		// the support graph (and power registration discipline inside the
		// solver) must forget its contribution.
		OwningStructure->RecomputeSupport();
	}
}

FTransform ABasePiece::GetSocketWorldTransform(FName SocketName) const
{
	if (Def)
	{
		for (const FPieceSocketDef& Socket : Def->Sockets)
		{
			if (Socket.SocketName == SocketName)
			{
				return Socket.LocalTransform * GetActorTransform();
			}
		}
	}
	return GetActorTransform();
}

bool ABasePiece::IsFunctional() const
{
	return Construction && Construction->IsComplete() && Health > 0.f;
}

// --- IInteractable -----------------------------------------------------------

FText ABasePiece::GetInteractionPrompt_Implementation() const
{
	return Def ? Def->DisplayName : NSLOCTEXT("Exoneer", "PiecePrompt", "Structure");
}

FGameplayTagContainer ABasePiece::GetInteractionTags_Implementation() const
{
	// Plain architecture offers no interaction verb; the prompt only labels
	// what the player is looking at. AMachinePiece overrides this.
	return FGameplayTagContainer();
}

// --- IConstructible (forwards to the Construction component) ----------------

EConstructionPhase ABasePiece::GetConstructionPhaseAt_Implementation(const FVector& WorldPoint) const
{
	// Whole-actor construction: WorldPoint is ignored by design.
	return Construction ? Construction->GetPhase() : EConstructionPhase::Complete;
}

float ABasePiece::GetConstructionProgressAt_Implementation(const FVector& WorldPoint) const
{
	return Construction ? Construction->GetTotalProgress01() : 1.f;
}

int32 ABasePiece::GetConstructionTargetIdAt_Implementation(const FVector& WorldPoint) const
{
	// Whole-actor construction: one target or none, so WorldPoint decides nothing.
	return Construction ? ExoneerConstruction::WholeActorTargetId : ExoneerConstruction::NoTargetId;
}

float ABasePiece::GetConstructionProgressForTarget_Implementation(int32 TargetId) const
{
	// A piece with no construction component, or an id from some other actor,
	// is "unknown" - not "finished". UConstructionComponent::InvestWork has no
	// equivalent ambiguity of its own: the component IS the single target, so
	// the identity is decided here, at the actor that owns it.
	if (!Construction || TargetId != ExoneerConstruction::WholeActorTargetId)
	{
		return ExoneerConstruction::UnknownProgress;
	}
	return Construction->GetTotalProgress01();
}

float ABasePiece::InvestConstruction_Implementation(AActor* Builder, UInventoryComponent* SourceInventory, const FVector& WorldPoint, float WeldPoints, int32& OutTargetId)
{
	OutTargetId = ExoneerConstruction::NoTargetId;
	if (!HasAuthority() || !Construction)
	{
		return 0.f;
	}
	const float Applied = Construction->InvestWork(SourceInventory, WeldPoints);
	if (Applied > 0.f)
	{
		OutTargetId = ExoneerConstruction::WholeActorTargetId;
	}
	return Applied;
}

float ABasePiece::DeconstructAt_Implementation(AActor* Builder, UInventoryComponent* RefundInventory, const FVector& WorldPoint, float WreckPoints)
{
	if (!HasAuthority() || !Construction)
	{
		return 0.f;
	}

	const float Applied = Construction->DeconstructWork(RefundInventory, WreckPoints);

	// A ghost wrecked down to zero progress (including a fresh, never-welded
	// ghost) disappears entirely.
	if (Construction->GetPhase() == EConstructionPhase::Ghost && Construction->GetTotalProgress01() <= 0.f)
	{
		if (OwningStructure)
		{
			OwningStructure->NotifyPieceRemoved(this);
		}
		Destroy();
	}
	return Applied;
}

// --- IDamageable -------------------------------------------------------------

bool ABasePiece::WipeDust()
{
	if (!HasAuthority() || Condition.SurfaceOpacity01 <= KINDA_SMALL_NUMBER)
	{
		return false;
	}
	Condition.SurfaceOpacity01 = 0.f;
	return true;
}

bool ABasePiece::IsConditionTerminal() const
{
	// One reading, one class: a battery bank at the fade floor. Nothing else
	// on a base piece has a spare, so nothing else can read terminal here.
	return Def && Def->EnergyStorage > 0.f
		&& ExoneerMaintenance::IsCapacityTerminal(Condition.CapacityFade01);
}

void ABasePiece::ResetConditionToNominal()
{
	Condition = FPartCondition();
	PendingDeflectionMm = 0.f;
	bLoadOverloaded = false;
	LastLoadN = 0.f;
	PendingLoadN = 0.f;
	PendingLoadFrames = 0;
}

bool ABasePiece::ReplacePart(UInventoryComponent* Source)
{
	// maintenance.md 4: the failure mode picks the verb. Replace spends a
	// FABRICATED spare, so it is legal only where the definition names one and
	// only at a terminal reading - never as a general-purpose refresh, and
	// never for dust (wipe already clears that in full).
	if (!HasAuthority() || !Source || !Def || Def->SpareItemId.IsNone())
	{
		return false;
	}
	if (!Construction || !Construction->IsComplete() || !IsConditionTerminal())
	{
		return false;
	}

	const FPrimaryAssetId SpareAsset(TEXT("Item"), Def->SpareItemId);
	UItemDefinitionDataAsset* Spare = Cast<UItemDefinitionDataAsset>(
		UAssetManager::Get().GetPrimaryAssetObject(SpareAsset));
	if (!Spare)
	{
		TSoftObjectPtr<UItemDefinitionDataAsset> Soft(UAssetManager::Get().GetPrimaryAssetPath(SpareAsset));
		Spare = Soft.LoadSynchronous();
	}
	if (!Spare || Source->GetItemCount(Spare) < 1)
	{
		return false;
	}
	if (Source->RemoveItem(Spare, 1) < 1)
	{
		return false;
	}

	// Placement, structure, parent and health are untouched: a cell swap is
	// not a rebuild and the weld tool still never heals (GAME-SCOPE 10).
	ResetConditionToNominal();
	return true;
}

/**
 * SERVER. Wheel load attribution. The wheel module calls this once per FRAME
 * per wheel standing on this piece (never once per physics substep, which
 * would multiply the load by the substep count). Several wheels inside one
 * frame add up, which is what a deck under an axle actually carries.
 */
void ABasePiece::ReportLiveLoad(float LoadN, uint64 FrameStamp)
{
	// Complete only: a ghost or half-welded piece carries nothing, so it can
	// neither sag nor collapse from load. The ghost is also transparent to the
	// wheel probe now, but the phase gate is the rule and the collision
	// response is only the optimisation.
	if (!HasAuthority() || LoadN <= 0.f || !Construction || !Construction->IsComplete())
	{
		return;
	}

	PendingLoadN += LoadN;
	if (LastLoadReportFrame != FrameStamp)
	{
		LastLoadReportFrame = FrameStamp;
		++PendingLoadFrames;
	}

	// Re-armed on every report: a structure merge under a parked rover moves
	// this piece to another structure, whose pass must then start servicing.
	if (OwningStructure)
	{
		OwningStructure->ArmLoadTimer();
	}
}

bool ABasePiece::IsLoadCondemned() const
{
	return Def && Def->TerminalDeflectionMm > 0.f && Condition.DeflectionMm >= Def->TerminalDeflectionMm;
}

void ABasePiece::ApplyWeatherWear(float StormIntensity, float DtSeconds)
{
	if (!HasAuthority() || !IsFunctional())
	{
		return;
	}
	Condition.SurfaceOpacity01 = FMath::Clamp(
		Condition.SurfaceOpacity01 + ExoneerMaintenance::DustOpacityDelta(StormIntensity, DtSeconds),
		0.f, 1.f);
}

float ABasePiece::ApplyExoneerDamage_Implementation(float Amount, EExoneerDamageType Type, AActor* DamageInstigator)
{
	// SERVER only; storm mitigation (StormResistance) is applied by the caller
	// (PlanetEnvironmentManager), so no double-dipping here.
	if (!HasAuthority() || Amount <= 0.f)
	{
		return 0.f;
	}

	const float Applied = FMath::Min(Amount, Health);
	Health -= Applied;

	if (Health <= 0.f)
	{
		if (OwningStructure)
		{
			OwningStructure->NotifyPieceRemoved(this);
		}
		Destroy();
	}
	return Applied;
}

float ABasePiece::GetMaxHealth_Implementation() const
{
	return Def ? Def->MaxHealth : 0.f;
}
