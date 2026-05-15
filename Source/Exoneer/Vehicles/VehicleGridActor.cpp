// Copyright Exoneer contributors.
#include "Vehicles/VehicleGridActor.h"
#include "Machines/CockpitBlock.h"
#include "Machines/ThrusterBlock.h"
#include "Building/BuildableBlock.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Pawn.h"

AVehicleGridActor::AVehicleGridActor()
{
	bIsVehicleGrid = true;
	PrimaryActorTick.bCanEverTick = true;

	PhysicsRoot = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PhysicsRoot"));
	SetRootComponent(PhysicsRoot);
	PhysicsRoot->SetMobility(EComponentMobility::Movable);
	PhysicsRoot->SetSimulatePhysics(true);
	PhysicsRoot->SetLinearDamping(0.5f);
	PhysicsRoot->SetAngularDamping(2.5f);
	PhysicsRoot->SetCollisionProfileName(TEXT("PhysicsActor"));
}

void AVehicleGridActor::SetActivePilot(APawn* InPilot, ACockpitBlock* Cockpit)
{
	ActivePilot = InPilot;
	ActiveCockpit = Cockpit;
	if (!InPilot)
	{
		PendingMove = FVector::ZeroVector;
		PendingRotate = FVector::ZeroVector;
	}
}

void AVehicleGridActor::ApplyPilotInput(const FVector& MoveInput, const FVector& RotateInput)
{
	PendingMove = MoveInput.GetClampedToMaxSize(1.f);
	PendingRotate = RotateInput.GetClampedToMaxSize(1.f);
}

void AVehicleGridActor::ApplyThrust(float DeltaSeconds)
{
	if (!PhysicsRoot) return;

	// Desired translation in local space — pilot input is interpreted relative
	// to the vehicle's own axes.
	const FVector DesiredLocal = PendingMove;
	const FTransform Xf = GetActorTransform();
	const FVector DesiredWorld = Xf.TransformVectorNoScale(DesiredLocal);

	// Walk every cockpit-owned thruster; throttle is proportional to alignment.
	TSet<ABuildableBlock*> Visited;
	FVector TotalForce = FVector::ZeroVector;
	for (const auto& KV : GetBlocks())
	{
		if (Visited.Contains(KV.Value)) continue;
		Visited.Add(KV.Value);
		if (AThrusterBlock* T = Cast<AThrusterBlock>(KV.Value))
		{
			const FVector ThrustDirWorld = Xf.TransformVectorNoScale(T->GetLocalThrustDirection());
			// Thruster pushes opposite of its nozzle: so it accelerates the
			// vehicle along +ThrustDirWorld when its nozzle faces -dir of motion.
			// We assign throttle when the desired direction is aligned to
			// the *forward* (thrust output) direction.
			const float Dot = FVector::DotProduct(DesiredWorld.GetSafeNormal(), ThrustDirWorld);
			T->Throttle = FMath::Clamp(Dot, 0.f, 1.f);
			TotalForce += T->GetWorldThrustForce();
		}
	}
	PhysicsRoot->AddForce(TotalForce);

	// Rotation: pilot input mapped to torque around local axes.
	const FVector TorqueLocal = PendingRotate * RotationTorque;
	const FVector TorqueWorld = Xf.TransformVectorNoScale(TorqueLocal);
	PhysicsRoot->AddTorqueInRadians(TorqueWorld);
}

void AVehicleGridActor::ApplyHover(float DeltaSeconds)
{
	if (!PhysicsRoot || HoverHeight <= 0.f) return;
	const FVector Start = GetActorLocation();
	const FVector End = Start - FVector(0.f, 0.f, HoverHeight * 2.f);
	FHitResult Hit;
	FCollisionQueryParams P(SCENE_QUERY_STAT(ExoneerHover), false, this);
	if (GetWorld()->LineTraceSingleByChannel(Hit, Start, End, ECC_WorldStatic, P))
	{
		const float Dist = Hit.Distance;
		const float Error = HoverHeight - Dist;
		const float Vz = PhysicsRoot->GetPhysicsLinearVelocity().Z;
		const float ForceZ = (Error * HoverStrength) - (Vz * HoverDamping);
		PhysicsRoot->AddForce(FVector(0.f, 0.f, FMath::Max(0.f, ForceZ)));
	}
}

void AVehicleGridActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!ActivePilot) return;
	ApplyHover(DeltaSeconds);
	ApplyThrust(DeltaSeconds);
}
