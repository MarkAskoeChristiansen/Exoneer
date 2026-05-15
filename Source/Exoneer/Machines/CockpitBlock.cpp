// Copyright Exoneer contributors.
#include "Machines/CockpitBlock.h"
#include "Vehicles/VehicleGridActor.h"
#include "GameFramework/Pawn.h"

ACockpitBlock::ACockpitBlock()
{
}

AVehicleGridActor* ACockpitBlock::GetVehicle() const
{
	return Cast<AVehicleGridActor>(GetOwningGrid());
}

bool ACockpitBlock::EnterPilot_Implementation(APawn* Pilot)
{
	if (CurrentPilot || !Pilot) return false;
	CurrentPilot = Pilot;
	if (AVehicleGridActor* V = GetVehicle()) V->SetActivePilot(Pilot, this);
	return true;
}

void ACockpitBlock::ExitPilot_Implementation(APawn* Pilot)
{
	if (CurrentPilot == Pilot)
	{
		CurrentPilot = nullptr;
		if (AVehicleGridActor* V = GetVehicle()) V->SetActivePilot(nullptr, nullptr);
	}
}

void ACockpitBlock::ApplyPilotInput_Implementation(const FVector& MoveInput, const FVector& RotateInput)
{
	if (AVehicleGridActor* V = GetVehicle()) V->ApplyPilotInput(MoveInput, RotateInput);
}

bool ACockpitBlock::OnInteract_Implementation(AActor* Interactor)
{
	APawn* Pawn = Cast<APawn>(Interactor);
	if (!Pawn) return false;
	return IPilotable::Execute_EnterPilot(this, Pawn);
}
