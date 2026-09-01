// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "TestRoverSpawner.generated.h"

/**
 * Spawns one ready-built, fully-welded test rover at its own location on
 * server BeginPlay - a playtest convenience so the first drive needs no
 * in-game construction. Skipped when any vehicle construct already exists
 * nearby (a previous session's rover, or one restored from a save).
 *
 * Layout: 8x3 frame deck, cockpit front, two charged batteries + solar on
 * top, two steering wheel assemblies front and two drive wheels rear
 * (wheelbase 1.25 m, track 1.0 m, about 1.6 t). Control mode starts in
 * Ground so W/S/A/D drive immediately.
 */
UCLASS()
class EXONEER_API ATestRoverSpawner : public AActor
{
	GENERATED_BODY()

public:
	ATestRoverSpawner();

	/** No new rover if a construct already exists within this range (uu). */
	UPROPERTY(EditAnywhere, Category = "Spawner")
	float ExistingConstructCheckRadius = 3000.f;

protected:
	virtual void BeginPlay() override;

	void SpawnRover();
};
