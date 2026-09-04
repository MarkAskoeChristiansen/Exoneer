// Copyright Exoneer contributors.
#include "Components/MiningToolComponent.h"
#include "Components/InventoryComponent.h"
#include "Components/SurvivalStatsComponent.h"
#include "Resources/ResourceNode.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Exoneer.h"

UMiningToolComponent::UMiningToolComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	// Required so Server_MineTarget can route through the owning pawn.
	SetIsReplicatedByDefault(true);
}

void UMiningToolComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* TickFn)
{
	Super::TickComponent(DeltaTime, TickType, TickFn);

	// The beam trace is local-player intent only; the server acts solely
	// through the validated Server_MineTarget RPC below.
	const APawn* Pawn = Cast<APawn>(GetOwner());
	if (!Pawn || !Pawn->IsLocallyControlled())
	{
		return;
	}
	if (!bActive)
	{
		IntentAccumulator = 0.f;
		return;
	}

	AActor* Owner = GetOwner();
	// The owner's eyes, not "the first UCameraComponent on the actor": a pawn
	// can carry more than one camera (visor plus chase boom) and that lookup
	// returns an arbitrary one. The server reach check uses the same point.
	FVector Start;
	FRotator ViewRot;
	Owner->GetActorEyesViewPoint(Start, ViewRot);
	const FVector End = Start + ViewRot.Vector() * Range;

	FHitResult Hit;
	FCollisionQueryParams Params(SCENE_QUERY_STAT(ExoneerMine), false, Owner);
	const bool bHit = GetWorld()->SweepSingleByChannel(Hit, Start, End, FQuat::Identity, ECC_Visibility,
		FCollisionShape::MakeSphere(Radius), Params);

	AResourceNode* Node = bHit ? Cast<AResourceNode>(Hit.GetActor()) : nullptr;
	if (!Node)
	{
		// Do not bank beam time spent aiming at nothing.
		IntentAccumulator = 0.f;
		return;
	}

	OnMiningProgress.Broadcast(Node->GetIntegrityFraction());

	// Batch the beam time and flush it as one intent at ~MineRateHz.
	IntentAccumulator += DeltaTime;
	const float FlushInterval = MineRateHz > 0.f ? 1.f / MineRateHz : 0.f;
	if (IntentAccumulator >= FlushInterval)
	{
		Server_MineTarget(Node, Hit.ImpactPoint, IntentAccumulator);
		IntentAccumulator = 0.f;
	}
}

bool UMiningToolComponent::Server_MineTarget_Validate(AResourceNode* Node, FVector_NetQuantize HitPoint, float Seconds)
{
	// Node may legitimately arrive null (destroyed in flight); only the
	// numeric payload can prove a tampered client.
	return FMath::IsFinite(Seconds) && Seconds >= 0.f && Seconds <= 2.f;
}

void UMiningToolComponent::Server_MineTarget_Implementation(AResourceNode* Node, FVector_NetQuantize HitPoint, float Seconds)
{
	AActor* Owner = GetOwner();
	if (!IsValid(Node) || !Owner || Seconds <= 0.f)
	{
		return;
	}

	// Range: both the claimed hit point and the node must sit within
	// Range * slack of the pawn's view point (bounds pad for large nodes).
	FVector ViewLoc;
	FRotator ViewRot;
	Owner->GetActorEyesViewPoint(ViewLoc, ViewRot);
	const float MaxDist = Range * ServerRangeSlack;
	if (FVector::DistSquared(ViewLoc, HitPoint) > FMath::Square(MaxDist))
	{
		return;
	}
	const float NodeBoundsRadius = Node->GetRootComponent() ? Node->GetRootComponent()->Bounds.SphereRadius : 0.f;
	if (FVector::DistSquared(ViewLoc, Node->GetActorLocation()) > FMath::Square(MaxDist + NodeBoundsRadius))
	{
		return;
	}

	// Wall-clock rate limit: a client cannot claim more beam time than has
	// actually passed (1.5x slack for latency jitter, banked up to 1 s).
	const double Now = GetWorld()->GetTimeSeconds();
	const double Elapsed = ServerLastMineTime > 0.0 ? Now - ServerLastMineTime : static_cast<double>(Seconds);
	ServerLastMineTime = Now;
	ServerDamageBudget = FMath::Min(ServerDamageBudget + static_cast<float>(Elapsed) * 1.5f, 1.f);
	Seconds = FMath::Min(Seconds, ServerDamageBudget);
	if (Seconds <= 0.f)
	{
		return;
	}
	ServerDamageBudget -= Seconds;

	// Suit power gates the beam and pays for it (authority only).
	USurvivalStatsComponent* Stats = Owner->FindComponentByClass<USurvivalStatsComponent>();
	if (Stats)
	{
		if (Stats->SuitPower <= 0.f)
		{
			return;
		}
		if (SuitPowerDrainPerSec > 0.f)
		{
			Seconds = FMath::Min(Seconds, Stats->SuitPower / SuitPowerDrainPerSec);
		}
	}
	if (Seconds <= 0.f)
	{
		return;
	}

	UInventoryComponent* Inventory = Owner->FindComponentByClass<UInventoryComponent>();
	Node->MineByPlayer(DamagePerSec * Seconds, Inventory);
	if (Stats)
	{
		Stats->AddSuitPower(-SuitPowerDrainPerSec * Seconds);
	}
}
