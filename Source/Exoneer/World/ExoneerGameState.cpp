// Copyright Exoneer contributors.
#include "World/ExoneerGameState.h"
#include "Net/UnrealNetwork.h"

AExoneerGameState::AExoneerGameState()
{
	bReplicates = true;
}

void AExoneerGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AExoneerGameState, Projects);
	DOREPLIFETIME(AExoneerGameState, Orbital);
	DOREPLIFETIME(AExoneerGameState, SolIndex);
}
