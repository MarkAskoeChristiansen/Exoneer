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
 * Layout: a 12 x 4 cell ladder chassis (3.00 m x 1.00 m) with two longerons
 * and four crossmembers; six wheels hang one cell outboard (track 1.25 m) on
 * three axles at 1.00 m spacing - front pair steers, all six drive. Module
 * bay down the centre: four charged batteries, one attitude gyro, two solar,
 * cockpit at the nose. Eight thrusters: six lifting (their centroid sits on
 * the centre of mass, so full lift produces no pitch or roll trim) and two
 * facing forward mounted BELOW the CoM, so forward thrust pitches the nose up
 * and self-limits instead of diverging. About 3.0 t.
 *
 * Control mode starts in Ground so W/S/A/D drive immediately; V switches to
 * Flight for the thrusters and gyro.
 */
UCLASS()
class EXONEER_API ATestRoverSpawner : public AActor
{
	GENERATED_BODY()

public:
	ATestRoverSpawner();
	virtual void Tick(float DeltaSeconds) override;

	/**
	 * SERVER test-range fixture. Seats the local engineer and flies a fixed
	 * 26-second climb/hover/translate/roll/yaw/descend profile through the real
	 * vehicle router and Chaos body. Console: exoneer.FlightProfile
	 */
	void StartAutomatedFlightProfile();

	/** No new rover if a construct already exists within this range (uu). */
	UPROPERTY(EditAnywhere, Category = "Spawner")
	float ExistingConstructCheckRadius = 3000.f;

	/**
	 * Actor-origin height on spawn (uu). The contact patch sits 52.5 uu below
	 * the actor origin at zero compression (wheel block centre +12.5, minus
	 * rest length 30, minus radius 35), and the garage pad top is at Z = -1,
	 * so 55 leaves a few uu of drop and the suspension settles instead of
	 * slamming its bump stops and bouncing a 3 t vehicle.
	 */
	UPROPERTY(EditAnywhere, Category = "Spawner")
	float SpawnHeightUU = 55.f;

protected:
	virtual void BeginPlay() override;

	void SpawnRover();

	TWeakObjectPtr<class AVehicleConstruct> SpawnedRover;
	TWeakObjectPtr<class APlayerSurvivalCharacter> FlightTestPilot;
	bool bFlightProfileActive = false;
	float FlightProfileTime = 0.f;
	float FlightProfileLogTime = 0.f;
	float FlightProfileMaxClimbMS = 0.f;
	float FlightProfileHoverVerticalMS = 0.f;
	float FlightProfileMaxForwardMS = 0.f;
	float FlightProfileMaxBankDeg = 0.f;
	float FlightProfileLevelBankDeg = 0.f;
	float FlightProfileYawTravelDeg = 0.f;
	float FlightProfileMinDescentMS = 0.f;
	float FlightProfileMaxGyro01 = 0.f;
	float FlightProfileMinEnergyKJ = TNumericLimits<float>::Max();
	float FlightProfileLastYawDeg = 0.f;
	bool bFlightProfileInverted = false;
};
