// Copyright Exoneer contributors.
//
// Unit tests for the pure thrust-routing library (Vehicles/ExoneerThrust.h):
// the lift valve, the lift governor, and the differential-thrust allocator.
//
// These exist because of specific playtest reports - "the gyro is not stable,
// it feels so weird to fly", then "now when i take off its like the thrusters
// keep firing and also its almost uncontrollable" - and they pin the
// properties that were lost, in the order they were lost:
//
//   1. Release the lift key and lift stops climbing. Nothing latches: there is
//      a key that closes the valve, touching down closes it, an input timeout
//      closes it, and leaving the seat closes it.
//   2. Released, the valve HOLDS ALTITUDE instead of choosing between 0.19 g
//      of climb and 1 g of fall. The governor has a rate term and no
//      integrator, so its whole state is a vertical speed the pilot can read.
//   3. An attitude command does not change altitude, and does not move a valve
//      at all: the trim request is a function of the craft's standing moment
//      and its rotor store, never of the rate command.
//   4. THE STANDING MOMENT IS CANCELLED BY THRUST, not by rotor momentum. This
//      is the one that made the shipped rover die 1.3 s into forward flight.
//
// The rover's thruster arms are DERIVED here from the spawned layout and the
// authored block masses rather than copied from a log, and
// Exoneer.Thrust.RoverGeometryIsTheShippedLayout checks the derivation against
// the mass the vehicle actually has. The previous version of this file carried
// hand-written arms that were wrong by half a cell in X, which is how a lift
// group 0.027 m off the centre of mass came to be described as centred on it.
//
// Headless run:
//   UnrealEditor-Cmd.exe <uproject> -ExecCmds="Automation RunTests Exoneer.Thrust; Quit"
//     -TestExit="Automation Test Queue Empty" -unattended -nullrhi -nosplash -nop4 -log

#include "Misc/AutomationTest.h"
#include "Vehicles/ExoneerThrust.h"
#include "Vehicles/ExoneerAttitude.h"
// The jet direction and the orientation table, so the fixture below models the
// thrusters the SPAWNER places rather than an idealised set. Modelling the
// forward pair as a plain FVector::ForwardVector is what let a 920 N*m standing
// yaw moment ship: every test in this file measured a rover that is not spawned.
#include "Vehicles/VehicleModule.h"
#include "Vehicles/VehicleOrientation.h"
#include "Data/VehicleBlockDefinitionDataAsset.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	using namespace ExoneerThrust;

	/** Authored on DA_Block_Thruster. */
	constexpr float ValveSlewPerSec = 2.f;
	constexpr float TrimFractionAuthored = 0.35f;
	constexpr float ThrusterMaxThrustN = 4000.f;
	constexpr float ReserveFractionAuthored = 0.1f;
	constexpr float HoverDampingAuthored = 0.1f;
	constexpr float NozzleCantDegAuthored = 6.f;
	constexpr float DescentRateAuthored = 2.5f;

	/** Authored on DA_Block_Gyro; the rover carries two triads. */
	constexpr float RoverRatedTorqueNm = 4000.f;      // 2 x 2000 N*m per axis
	constexpr float RoverCapacityNms = 1600.f;        // 2 x 800 N*m*s per axis
	constexpr float RoverDumpOnsetFraction = 0.8f;
	constexpr float RoverDumpReleaseFraction = 0.4f;
	constexpr float RoverDumpRatePerSec = 0.35f;
	constexpr float RoverSettleTimeSeconds = 0.25f;
	constexpr float RoverRateCeilingDegPerSec = 20.f;
	constexpr float RoverCommandMomentumFraction = 0.5f;
	constexpr float RoverSustainedTurnSeconds = 4.f;
	constexpr float RoverOffloadTimeConstant = 1.5f;

	/** Measured properties of the 6x6 test rover. */
	constexpr float RoverMassKg = 1849.f;
	constexpr float PlanetGravityMS2 = 9.8f;
	const FVector RoverInertiaKgM2(308.3f, 1354.8f, 1543.8f);
	/** Chaos angular damping on the hull (1/s). This is what a HOLD costs. */
	constexpr float RoverHullAngularDamping = 0.15f;

	/** Build grid pitch (m). AVehicleConstruct::CellSize is 25 cm. */
	constexpr float CellM = 0.25f;

	/**
	 * The pilot's throttle ceiling and the hover setting that follow.
	 *
	 * LIFT IS COS^2 OF THE NOZZLE CANT, not the installed thrust. The router
	 * hands each unit AlongLift * collective of base throttle and then only
	 * AlongWorldUp of that pushes up, so a level craft with every nozzle toed
	 * out by 6 degrees makes 6 * 4000 * cos^2(6 deg) = 23.74 kN of vertical
	 * lift out of 24.0 kN installed. That 1.1 percent is what the yaw axis
	 * costs and it shows on the TWR readout.
	 */
	const float RoverCeiling = PilotThrottleCeiling(ReserveFractionAuthored);
	const float RoverCantCos = FMath::Cos(FMath::DegreesToRadians(NozzleCantDegAuthored));
	const float RoverLiftScaleN = 6.f * ThrusterMaxThrustN * RoverCantCos * RoverCantCos;
	const float RoverWeightN = RoverMassKg * PlanetGravityMS2;               // 18.12 kN
	const float RoverHoverCollective = RoverWeightN / RoverLiftScaleN;       // 0.763

	/**
	 * ONE BLOCK OF THE SPAWNED ROVER, as ATestRoverSpawner places it: a cell
	 * origin, a size in cells, and the mass its definition authors. Deriving
	 * the centre of mass from this is the only way the arms below can be
	 * trusted, and it is what turns "the lift centroid lands on the centre of
	 * mass" from a comment into a number.
	 */
	struct FRoverBlock
	{
		FIntVector Origin = FIntVector::ZeroValue;
		FIntVector Size = FIntVector(1, 1, 1);
		float MassKg = 0.f;
	};

	/**
	 * ONE THRUSTER AS THE SPAWNER PLACES IT: a cell origin and the ORIENTATION
	 * index, which is the whole point. Every thruster carries the nozzle cant
	 * (it is on the block definition), so four orientations aim a unit the same
	 * way and differ only in which way the jet leans - and that lean is where
	 * the craft's yaw authority comes from and where 920 N*m of uncancellable
	 * standing yaw came from when both forward units leaned the same way.
	 *
	 * The indices come from the SAME two-axis helper ATestRoverSpawner and
	 * UBuildToolComponent call, so this fixture cannot drift from the vehicle:
	 * if the spawner changes which orientation it passes, these change with it.
	 */
	struct FRoverThruster
	{
		FIntVector Origin = FIntVector::ZeroValue;
		uint8 Orientation = 0;
	};

	uint8 AimWithToe(const FVector& Target, const FVector& Lean)
	{
		return ExoneerVehicleOrientation::FindOrientationMappingAxes(
			ExoneerThruster::LocalThrustAxis, Target, FVector::YAxisVector, Lean);
	}
	uint8 ThrustUpToeLeft()       { return AimWithToe(FVector::UpVector, -FVector::YAxisVector); }
	uint8 ThrustUpToeRight()      { return AimWithToe(FVector::UpVector, FVector::YAxisVector); }
	uint8 ThrustForwardToeLeft()  { return AimWithToe(FVector::ForwardVector, -FVector::YAxisVector); }
	uint8 ThrustForwardToeRight() { return AimWithToe(FVector::ForwardVector, FVector::YAxisVector); }

	/** The eight thrusters ATestRoverSpawner places, in placement order. */
	TArray<FRoverThruster> ShippedRoverThrusters()
	{
		TArray<FRoverThruster> Out;
		for (const int32 X : { 2, 5, 8 })
		{
			Out.Add({ FIntVector(X, 0, 1), ThrustUpToeLeft() });
			Out.Add({ FIntVector(X, 3, 1), ThrustUpToeRight() });
		}
		// MIRRORED, which is the fix: the unit on the -Y side of the centre of
		// mass toes left, the one on the +Y side toes right, so the two 418 N
		// lateral components and the two x_off * F_y yaw moments cancel exactly.
		Out.Add({ FIntVector(0, 1, 0), ThrustForwardToeLeft() });
		Out.Add({ FIntVector(0, 2, 1), ThrustForwardToeRight() });
		return Out;
	}

	/**
	 * The DEFECT this round closed: both forward units on the one-axis lookup
	 * FindOrientationMappingAxis(LocalThrustAxis, ForwardVector), which returns
	 * the first of the four matching orientations and therefore leans both the
	 * same way. Kept so the fix is measured against it rather than asserted.
	 */
	TArray<FRoverThruster> OneAxisForwardRoverThrusters()
	{
		TArray<FRoverThruster> Out = ShippedRoverThrusters();
		const uint8 OneAxis = ExoneerVehicleOrientation::FindOrientationMappingAxis(
			ExoneerThruster::LocalThrustAxis, FVector::ForwardVector);
		Out[Out.Num() - 2].Orientation = OneAxis;
		Out[Out.Num() - 1].Orientation = OneAxis;
		return Out;
	}

	/** Every lift nozzle toed the SAME way: what the old six-entry aim list gave a player. */
	TArray<FRoverThruster> UniformToeRoverThrusters()
	{
		TArray<FRoverThruster> Out = ShippedRoverThrusters();
		for (int32 Index = 0; Index < 6; ++Index)
		{
			Out[Index].Orientation = ThrustUpToeLeft();
		}
		return Out;
	}

	/**
	 * The shipped layout. Mirrors ATestRoverSpawner::SpawnRover, block for
	 * block, with the thruster cells taken from the table above so a layout
	 * change cannot move the mass without moving the jets.
	 */
	TArray<FRoverBlock> RoverLayout(const TArray<FRoverThruster>& Thrusters)
	{
		TArray<FRoverBlock> Out;
		auto Add = [&Out](FIntVector Origin, FIntVector Size, float MassKg)
		{
			Out.Add({ Origin, Size, MassKg });
		};
		// Ladder chassis, 32 frames at 14 kg.
		Add(FIntVector(0, 0, 0), FIntVector(1, 1, 1), 14.f);
		for (int32 X = 1; X <= 11; ++X)
		{
			Add(FIntVector(X, 0, 0), FIntVector(1, 1, 1), 14.f);
		}
		Add(FIntVector(1, 1, 0), FIntVector(1, 1, 1), 14.f);
		Add(FIntVector(1, 2, 0), FIntVector(1, 1, 1), 14.f);
		for (int32 X = 1; X <= 11; ++X)
		{
			Add(FIntVector(X, 3, 0), FIntVector(1, 1, 1), 14.f);
		}
		Add(FIntVector(0, 3, 0), FIntVector(1, 1, 1), 14.f);
		for (const int32 X : { 5, 9, 11 })
		{
			Add(FIntVector(X, 1, 0), FIntVector(1, 1, 1), 14.f);
			Add(FIntVector(X, 2, 0), FIntVector(1, 1, 1), 14.f);
		}
		// Six road wheels, 3x1x3 cells at 60 kg.
		for (const FIntVector& Origin : { FIntVector(0, -1, -1), FIntVector(0, 4, -1),
			FIntVector(4, -1, -1), FIntVector(4, 4, -1), FIntVector(8, -1, -1), FIntVector(8, 4, -1) })
		{
			Add(Origin, FIntVector(3, 1, 3), 60.f);
		}
		// Four batteries at 50 kg, two triads at 180 kg, two solar at 18 kg, cockpit 85 kg.
		for (const FIntVector& Origin : { FIntVector(1, 1, 1), FIntVector(1, 2, 1),
			FIntVector(2, 1, 1), FIntVector(2, 2, 1) })
		{
			Add(Origin, FIntVector(1, 1, 1), 50.f);
		}
		Add(FIntVector(5, 1, 1), FIntVector(2, 2, 2), 180.f);
		Add(FIntVector(7, 1, 1), FIntVector(2, 2, 2), 180.f);
		Add(FIntVector(4, 1, 1), FIntVector(1, 1, 1), 18.f);
		Add(FIntVector(4, 2, 1), FIntVector(1, 1, 1), 18.f);
		Add(FIntVector(11, 1, 1), FIntVector(1, 1, 1), 85.f);
		// Eight thrusters at 45 kg, taken from the same table the effectors are
		// built from, so a layout change cannot move the mass without moving
		// the jets. (A 1x1x1 block occupies the same cell at any orientation,
		// so the mass properties do not depend on the toe.)
		for (const FRoverThruster& Thruster : Thrusters)
		{
			Add(Thruster.Origin, FIntVector(1, 1, 1), 45.f);
		}
		return Out;
	}

	TArray<FRoverBlock> ShippedRoverLayout()
	{
		return RoverLayout(ShippedRoverThrusters());
	}

	/** Centre of a block's cell span, in metres from the chassis origin corner. */
	FVector BlockCentreM(const FRoverBlock& Block)
	{
		return FVector(
			(Block.Origin.X + Block.Size.X * 0.5f) * CellM,
			(Block.Origin.Y + Block.Size.Y * 0.5f) * CellM,
			(Block.Origin.Z + Block.Size.Z * 0.5f) * CellM);
	}

	/** Mass and centre of mass of a layout (kg, m). */
	void LayoutMassProperties(const TArray<FRoverBlock>& Layout, float& OutMassKg, FVector& OutCentreM)
	{
		double Mass = 0.0;
		FVector Moment = FVector::ZeroVector;
		for (const FRoverBlock& Block : Layout)
		{
			Mass += Block.MassKg;
			Moment += BlockCentreM(Block) * Block.MassKg;
		}
		OutMassKg = static_cast<float>(Mass);
		OutCentreM = Mass > 0.0 ? Moment / Mass : FVector::ZeroVector;
	}

	/**
	 * The rover's thruster set as the allocator sees it, built the way
	 * AVehicleConstruct::ServerRouteThrust builds it: EVERY unit's jet is
	 * ExoneerThruster::LocalThrustDirection(cant) rotated by the ORIENTATION
	 * the spawner actually passes, with the arms in metres from the DERIVED
	 * centre of mass.
	 *
	 * THIS IS THE LINE THAT WAS WRONG, and it is why no test caught the yaw
	 * defect. The previous fixture modelled the forward pair as a plain
	 * FVector::ForwardVector - no cant at all - so every test in this file
	 * measured a rover that is not spawned. The real pair carried 6 degrees of
	 * cant leaning the SAME way, which is 418 N of side thrust each, 920 N*m of
	 * standing yaw at the reserved ceiling against about 400 N*m of yaw trim
	 * authority, and a yaw axis that filled 3.3 s after the pilot first held W.
	 * Building the jet from the orientation index means the fixture and the
	 * spawner cannot disagree again: change the spawner and this moves with it.
	 *
	 * Thrusters is the layout to model, so the defective and naive-build
	 * arrangements can be measured rather than described.
	 */
	TArray<FTrimEffector> MakeRoverThrustersFrom(const TArray<FRoverThruster>& Thrusters,
		float LiftSetting, float ForwardDemand, float SupplyFraction = 1.f,
		float BankDeg = 0.f, float PitchDeg = 0.f)
	{
		float MassKg = 0.f;
		FVector CentreM = FVector::ZeroVector;
		LayoutMassProperties(RoverLayout(Thrusters), MassKg, CentreM);

		// THE CRAFT'S OWN ATTITUDE, so the governor, the trim and the lift
		// scale can all be evaluated on a BANKED craft. Every version of this
		// fixture before the banked cases was level-only, which is exactly why
		// no test reached the governor's behaviour past 33 degrees of bank.
		const FQuat Attitude(FRotator(PitchDeg, 0.f, BankDeg));
		const float ThrustN = ThrusterMaxThrustN * SupplyFraction;
		const float Ceiling = PilotThrottleCeiling(ReserveFractionAuthored);
		TArray<FTrimEffector> Out;
		for (const FRoverThruster& Thruster : Thrusters)
		{
			const FVector DirCraft = ExoneerVehicleOrientation::GetQuat(Thruster.Orientation)
				.RotateVector(ExoneerThruster::LocalThrustDirection(NozzleCantDegAuthored))
				.GetSafeNormal();
			const FVector Dir = Attitude.RotateVector(DirCraft);
			const FRoverBlock Block{ Thruster.Origin, FIntVector(1, 1, 1), 45.f };
			const FVector ArmM = Attitude.RotateVector(BlockCentreM(Block) - CentreM);
			const FVector LiftAxis = Attitude.GetAxisZ();
			// The pilot's two demands, resolved on the jet exactly as the router
			// resolves them: lift by the unit's share of the craft's own lift
			// axis, forward by its share of the craft's forward axis. A canted
			// unit therefore gets cos(cant) of what it asks for, which is where
			// the cost of the toe shows up.
			const float AlongLift = FMath::Max(0.f,
				static_cast<float>(FVector::DotProduct(DirCraft, FVector::UpVector)));
			const float AlongForward = FMath::Max(0.f,
				static_cast<float>(FVector::DotProduct(DirCraft, FVector::ForwardVector)));

			FTrimEffector Effector;
			Effector.MomentPerUnitThrottle = FVector::CrossProduct(ArmM, Dir * ThrustN);
			Effector.ForcePerUnitThrottle = Dir * ThrustN;
			Effector.LiftPerUnitThrottle = ThrustN * FMath::Max(0.f,
				static_cast<float>(FVector::DotProduct(Dir, LiftAxis)));
			Effector.BaseThrottle = FMath::Clamp(
				AlongForward * ForwardDemand + AlongLift * LiftSetting, 0.f, Ceiling);
			Effector.TrimFraction = TrimFractionAuthored;
			Out.Add(Effector);
		}
		return Out;
	}

	TArray<FTrimEffector> MakeRoverThrusters(float LiftSetting, float ForwardDemand, float SupplyFraction = 1.f,
		float BankDeg = 0.f, float PitchDeg = 0.f)
	{
		return MakeRoverThrustersFrom(ShippedRoverThrusters(), LiftSetting, ForwardDemand,
			SupplyFraction, BankDeg, PitchDeg);
	}

	/**
	 * World-VERTICAL lift the craft makes at collective 1 (N) at a given
	 * attitude - the quantity the hover governor divides weight by. Same sum
	 * the router builds: thrust x lift-axis share x world-up share.
	 */
	float RoverVerticalLiftScaleN(float BankDeg = 0.f, float PitchDeg = 0.f,
		const TArray<FRoverThruster>* Thrusters = nullptr)
	{
		const TArray<FTrimEffector> Set = MakeRoverThrustersFrom(
			Thrusters ? *Thrusters : ShippedRoverThrusters(), 0.f, 0.f, 1.f, BankDeg, PitchDeg);
		const FQuat Attitude(FRotator(PitchDeg, 0.f, BankDeg));
		const FVector LiftAxis = Attitude.GetAxisZ();
		double Total = 0.0;
		for (const FTrimEffector& Effector : Set)
		{
			const FVector Dir = Effector.ForcePerUnitThrottle.GetSafeNormal();
			const double AlongLift = FMath::Max(0.0, FVector::DotProduct(Dir, LiftAxis));
			// SIGNED on the world-up factor, exactly as the router now sums it.
			// Clamped at zero the sum can only ever answer "opening the valve
			// raises the craft", which is false past 90 degrees of bank - and
			// that is how the governor came to freeze the valve at the ceiling
			// while 21.1 kN of lift pointed at the ground.
			const double AlongUp = FVector::DotProduct(Dir, FVector::UpVector);
			Total += Effector.ForcePerUnitThrottle.Size() * AlongLift * AlongUp;
		}
		return static_cast<float>(Total);
	}

	/** The two axes a trim may not move the craft along: lift, then forward. */
	void RoverNullAxes(FVector Out[2], float BankDeg = 0.f, float PitchDeg = 0.f)
	{
		const FQuat Attitude(FRotator(PitchDeg, 0.f, BankDeg));
		Out[0] = Attitude.GetAxisZ();
		Out[1] = Attitude.GetAxisX();
	}

	/**
	 * The rover as it USED to be built, two designs ago: NO nozzle cant at all
	 * (every jet exactly along its own aim axis) and the forward pair both at
	 * Z = 0, wholly below the centre of mass. Kept so the fix can be measured
	 * against the defect rather than asserted. Self-contained on purpose - it
	 * models a craft that no longer exists, so it must not drift with the
	 * shipped tables.
	 */
	TArray<FTrimEffector> MakePreviousRoverThrusters(float LiftSetting, float ForwardDemand)
	{
		TArray<FRoverThruster> Placements;
		for (const int32 X : { 2, 5, 8 })
		{
			Placements.Add({ FIntVector(X, 0, 1), 0 });
			Placements.Add({ FIntVector(X, 3, 1), 0 });
		}
		Placements.Add({ FIntVector(0, 1, 0), 0 });
		Placements.Add({ FIntVector(0, 2, 0), 0 });

		float MassKg = 0.f;
		FVector CentreM = FVector::ZeroVector;
		LayoutMassProperties(RoverLayout(Placements), MassKg, CentreM);

		TArray<FTrimEffector> Out;
		for (const FRoverThruster& Placement : Placements)
		{
			const bool bForward = Placement.Origin.X == 0;
			const FVector Dir = bForward ? FVector::ForwardVector : FVector::UpVector;
			const FRoverBlock Block{ Placement.Origin, FIntVector(1, 1, 1), 45.f };
			FTrimEffector Effector;
			Effector.MomentPerUnitThrottle = FVector::CrossProduct(BlockCentreM(Block) - CentreM, Dir * ThrusterMaxThrustN);
			Effector.ForcePerUnitThrottle = Dir * ThrusterMaxThrustN;
			Effector.LiftPerUnitThrottle = ThrusterMaxThrustN * FMath::Max(0.f, static_cast<float>(Dir.Z));
			// No reserve either: the previous build let the pilot reach the stop.
			Effector.BaseThrottle = FMath::Clamp(bForward ? ForwardDemand : LiftSetting, 0.f, 1.f);
			Effector.TrimFraction = TrimFractionAuthored;
			Out.Add(Effector);
		}
		return Out;
	}

	/**
	 * NET WORLD-VERTICAL FORCE at a given valve setting and attitude (N). The
	 * quantity the governor exists to control, and the one the B13 case is
	 * argued in: a valve setting is only "frozen" rather than "diving" if the
	 * force it makes still points up.
	 */
	float RoverVerticalForceN(float Valve, float BankDeg = 0.f, float PitchDeg = 0.f,
		const TArray<FRoverThruster>* Thrusters = nullptr)
	{
		const TArray<FTrimEffector> Set = MakeRoverThrustersFrom(
			Thrusters ? *Thrusters : ShippedRoverThrusters(), Valve, 0.f, 1.f, BankDeg, PitchDeg);
		double Total = 0.0;
		for (const FTrimEffector& Effector : Set)
		{
			Total += Effector.ForcePerUnitThrottle.Z * FMath::Clamp(Effector.BaseThrottle, 0.f, 1.f);
		}
		return static_cast<float>(Total);
	}

	/** Net force the pilot's own BASE throttles make along one axis (N), trims excluded. */
	float BaseForceAlongN(const TArray<FTrimEffector>& Effectors, const FVector& Axis)
	{
		double Total = 0.0;
		for (const FTrimEffector& Effector : Effectors)
		{
			Total += FVector::DotProduct(Effector.ForcePerUnitThrottle, Axis) * Effector.BaseThrottle;
		}
		return static_cast<float>(Total);
	}

	/** Total lift the set produces at its current throttles plus trims (N). */
	float TotalLiftN(const TArray<FTrimEffector>& Effectors)
	{
		double Total = 0.0;
		for (const FTrimEffector& Effector : Effectors)
		{
			Total += static_cast<double>(Effector.LiftPerUnitThrottle)
				* FMath::Clamp(Effector.BaseThrottle + Effector.Trim, 0.f, 1.f);
		}
		return static_cast<float>(Total);
	}

	/**
	 * ONE FRAME OF THE ROUTER'S TRIM PIPELINE, exactly as
	 * AVehicleConstruct::ServerRouteThrust runs it: the request is the craft's
	 * own standing moment with a minus sign, plus an unwind if the rotors are
	 * past the authored onset; the whole vector is rate-limited from last
	 * frame's bias; and the net lift is nulled before anything is committed.
	 *
	 * Returns the moment the rotors are left holding, which is the number the
	 * visor turns into a countdown.
	 */
	FVector RouteTrimFrame(TArray<FTrimEffector>& Effectors, const FVector& StoredMomentumNms,
		const FVector& AttitudeTorqueNm, float DeltaSeconds,
		const FVector& SustainedTorqueNm = FVector::ZeroVector, bool* DesaturatingLatch = nullptr,
		const FVector* NullAxes = nullptr, FVector* OutRequest = nullptr,
		bool bStandingTrimActive = true, bool bAirTrimActive = true)
	{
		FVector DefaultAxes[2];
		RoverNullAxes(DefaultAxes);
		const FVector* Axes = NullAxes ? NullAxes : DefaultAxes;

		for (FTrimEffector& Effector : Effectors)
		{
			Effector.PreviousTrim = Effector.Trim;
			Effector.Trim = 0.f;
		}
		const FVector Standing = StandingMomentNm(Effectors);
		// THE SUSTAINED PART OF THE ATTITUDE COMMAND rides here with the
		// standing moment, because both are torques that do not decay and a
		// rotor paying either one fills on a stopwatch.
		//
		// TWO GATES, as the router has: the standing-moment cancellation runs
		// whenever the VALVE IS OPEN, ground contact or not, while the rate
		// offload and the dump wait until the craft is airborne. Gating the
		// standing term on airborne is what spent 287 N*m*s of pitch during a
		// 0.75 s ground roll and saturated the axis outright on a craft that
		// lingered on the pad with the lift key held.
		FVector Request = FVector::ZeroVector;
		if (bStandingTrimActive)
		{
			Request -= Standing;
		}
		if (bAirTrimActive)
		{
			Request += SustainedTorqueNm;
		}

		const float Saturation = static_cast<float>(StoredMomentumNms.GetAbsMax()) / RoverCapacityNms;
		bool bLatched = DesaturatingLatch ? *DesaturatingLatch : false;
		bLatched = bAirTrimActive && ShouldDesaturate(Saturation, RoverDumpOnsetFraction,
			RoverDumpReleaseFraction, RoverDumpRatePerSec, bLatched);
		if (DesaturatingLatch)
		{
			*DesaturatingLatch = bLatched;
		}
		if (bLatched)
		{
			// Headroom is measured from what the triad will REALLY deliver,
			// which is why the pre-saturation ask is put through the same
			// per-axis saturation the block applies.
			const FVector Ask = (AttitudeTorqueNm - Standing).BoundToCube(RoverRatedTorqueNm);
			const FVector Deliverable = ExoneerAttitude::ApplySaturation(Ask, StoredMomentumNms, RoverCapacityNms);
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				const float Headroom = FMath::Max(0.f,
					RoverRatedTorqueNm - FMath::Abs(static_cast<float>(Deliverable[Axis])));
				Request[Axis] -= FMath::Clamp(static_cast<float>(StoredMomentumNms[Axis] * RoverDumpRatePerSec),
					-Headroom, Headroom);
			}
		}
		if (OutRequest)
		{
			*OutRequest = Request;
		}
		AllocateForceNeutralTrim(Effectors, Request, Axes, 2);
		SlewTrimTowardsRequest(Effectors, DeltaSeconds, ValveSlewPerSec);
		ClampTrimsToBounds(Effectors);
		EnforceForceNeutralTrim(Effectors, Axes, 2);
		return Standing + DeliveredTrimTorqueNm(Effectors);
	}

	/**
	 * TRUE vertical force at the current throttles (N), from the force vector
	 * rather than from LiftPerUnitThrottle. The two agree on any craft whose
	 * lift units point upward - every real one - and differ on arbitrary
	 * geometry, because LiftPerUnitThrottle carries the max(0, .) clamp the
	 * COLLECTIVE distribution needs and the physics does not.
	 */
	float TotalVerticalForceN(const TArray<FTrimEffector>& Effectors)
	{
		double Total = 0.0;
		for (const FTrimEffector& Effector : Effectors)
		{
			Total += Effector.ForcePerUnitThrottle.Z * FMath::Clamp(Effector.BaseThrottle + Effector.Trim, 0.f, 1.f);
		}
		return static_cast<float>(Total);
	}

	/** Sum of every unit's force magnitude per unit throttle (N). */
	float TrimForceScaleN(const TArray<FTrimEffector>& Effectors)
	{
		double Total = 0.0;
		for (const FTrimEffector& Effector : Effectors)
		{
			Total += Effector.ForcePerUnitThrottle.Size();
		}
		return static_cast<float>(Total);
	}

	/** Worst-axis force the trims leak along the axes the null does NOT hold. */
	float TrimLateralLeakN(const TArray<FTrimEffector>& Effectors, float BankDeg = 0.f, float PitchDeg = 0.f)
	{
		const FQuat Attitude(FRotator(PitchDeg, 0.f, BankDeg));
		return FMath::Abs(NetTrimForceAlongN(Effectors, Attitude.GetAxisY()));
	}
}

/**
 * The derivation the rest of this file leans on. If the layout table here and
 * the spawner ever disagree, the rover's mass will not come out at the 1849 kg
 * the vehicle actually has, and every arm below is then wrong.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerThrustRoverGeometryIsTheShippedLayout, "Exoneer.Thrust.RoverGeometryIsTheShippedLayout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerThrustRoverGeometryIsTheShippedLayout::RunTest(const FString& Parameters)
{
	const TArray<FRoverBlock> Layout = ShippedRoverLayout();
	float MassKg = 0.f;
	FVector CentreM = FVector::ZeroVector;
	LayoutMassProperties(Layout, MassKg, CentreM);

	// 1849 kg of welded blocks. The VEHICLE weighs 1850 kg, because the
	// construct's own root body carries 1 kg on top of the block list - said
	// out loud here rather than left as a one-kilogram discrepancy between a
	// hand-typed constant and the thing it claims to describe.
	TestEqual(FString::Printf(TEXT("the layout masses out at the vehicle's own %.0f kg of blocks"), MassKg),
		MassKg, RoverMassKg, 1.f);
	const TArray<FRoverThruster> Thrusters = ShippedRoverThrusters();
	TestEqual(TEXT("eight thrusters are installed"), Thrusters.Num(), 8);

	// EVERY JET, BY DIRECTION AND BY ORIENTATION INDEX. This is the assertion
	// whose absence let the yaw defect ship: the previous version of this test
	// checked the forward pair's Z straddle and the lift centroid and never once
	// looked at which way a nozzle actually points, so it could not have caught
	// a fixture that modelled the forward pair as uncanted or a spawner that
	// leaned both units the same way.
	{
		const float Cant = FMath::DegreesToRadians(NozzleCantDegAuthored);
		const float CantSin = FMath::Sin(Cant);
		const float CantCos = FMath::Cos(Cant);
		int32 LiftUnits = 0;
		int32 ForwardUnits = 0;
		for (const FRoverThruster& Thruster : Thrusters)
		{
			const FVector Jet = ExoneerVehicleOrientation::GetQuat(Thruster.Orientation)
				.RotateVector(ExoneerThruster::LocalThrustDirection(NozzleCantDegAuthored))
				.GetSafeNormal();
			const bool bForward = Thruster.Origin.X == 0;
			// The toe SIGN is set by which side of the centre of mass the unit
			// sits on, for the lift rails and the forward pair alike: outboard
			// on the rails is what makes the yaw couple, mirrored on the forward
			// pair is what makes their side thrust cancel.
			const float ExpectedToe = (BlockCentreM({ Thruster.Origin, FIntVector(1, 1, 1), 45.f }).Y < CentreM.Y)
				? -CantSin : CantSin;
			const FVector Expected = bForward
				? FVector(CantCos, ExpectedToe, 0.f)
				: FVector(0.f, ExpectedToe, CantCos);
			TestTrue(FString::Printf(TEXT("thruster at (%d,%d,%d) orientation %d aims (%+.3f,%+.3f,%+.3f)"),
					Thruster.Origin.X, Thruster.Origin.Y, Thruster.Origin.Z, Thruster.Orientation,
					Jet.X, Jet.Y, Jet.Z),
				Jet.Equals(Expected, 1e-3f));
			if (bForward)
			{
				++ForwardUnits;
			}
			else
			{
				++LiftUnits;
			}
		}
		TestEqual(TEXT("six units lift"), LiftUnits, 6);
		TestEqual(TEXT("and two face forward"), ForwardUnits, 2);

		// THE FORWARD PAIR IS A MIRRORED PAIR, and these are the two numbers
		// that say so. Both units sit 1.277 m behind the centre of mass, so a
		// lateral component that does NOT cancel is a standing yaw moment with
		// nothing to answer it - flight has no lateral thrust command at all.
		const TArray<FRoverThruster> OneAxis = OneAxisForwardRoverThrusters();
		const TArray<FTrimEffector> Mirrored = MakeRoverThrusters(0.f, 1.f);
		const TArray<FTrimEffector> Uncancelled = MakeRoverThrustersFrom(OneAxis, 0.f, 1.f);
		const float MirroredSide = BaseForceAlongN(Mirrored, FVector::YAxisVector);
		const float DefectSide = BaseForceAlongN(Uncancelled, FVector::YAxisVector);
		const float MirroredYaw = static_cast<float>(StandingMomentNm(Mirrored).Z);
		const float DefectYaw = static_cast<float>(StandingMomentNm(Uncancelled).Z);
		TestTrue(FString::Printf(
				TEXT("the one-axis lookup leaned both forward units the same way: %.0f N of side thrust, %.0f N*m of standing yaw"),
				DefectSide, DefectYaw),
			FMath::Abs(DefectSide) > 700.f && FMath::Abs(DefectYaw) > 850.f);
		TestTrue(FString::Printf(TEXT("mirrored, the side thrust cancels to %.2f N"), MirroredSide),
			FMath::Abs(MirroredSide) < 1.f);
		TestTrue(FString::Printf(TEXT("and the standing yaw falls to %.1f N*m"), MirroredYaw),
			FMath::Abs(MirroredYaw) < 60.f);

		// AND THE DIFFERENCE IS THE WHOLE DEFECT: 920 N*m against about 400 N*m
		// of yaw trim authority is a yaw axis that fills on a stopwatch, while
		// 41 N*m is trimmed to nothing (see StandingMomentIsCancelledByThrust).
		FVector Axes[2];
		RoverNullAxes(Axes);
		TArray<FTrimEffector> DefectTrimmed = MakeRoverThrustersFrom(OneAxis, RoverHoverCollective, RoverCeiling);
		const FVector DefectStanding = StandingMomentNm(DefectTrimmed);
		AllocateForceNeutralTrim(DefectTrimmed, -DefectStanding, Axes, 2);
		const float DefectResidualYaw = FMath::Abs(
			static_cast<float>((DefectStanding + DeliveredTrimTorqueNm(DefectTrimmed)).Z));
		const float SecondsToFillYaw = RoverCapacityNms / FMath::Max(DefectResidualYaw, 1.f);
		TestTrue(FString::Printf(
				TEXT("untrimmable: %.0f N*m of residual yaw fills the axis in %.1f s of holding W"),
				DefectResidualYaw, SecondsToFillYaw),
			DefectResidualYaw > 300.f && SecondsToFillYaw < 6.f);
	}

	// THE NOZZLE CANT, which is where this craft's in-air yaw authority comes
	// from and therefore the single geometric fact the flight fix rests on.
	{
		// (a) Uniform throttle makes NO yaw. If it did, the craft would be a
		//     windmill with a standing yaw moment at every hover setting -
		//     which is what a fore/aft toe pattern gives, and why the rails are
		//     toed instead.
		const TArray<FTrimEffector> AtHover = MakeRoverThrusters(RoverHoverCollective, 0.f);
		const FVector Standing = StandingMomentNm(AtHover);
		TestTrue(FString::Printf(TEXT("uniform throttle makes no standing yaw (%.2f N*m)"), Standing.Z),
			FMath::Abs(Standing.Z) < 1.f);

		// (b) A DIAGONAL trim across the four corner units is a pure yaw
		//     couple: yaw, no force, no pitch, no roll.
		TArray<FTrimEffector> Diagonal = MakeRoverThrusters(RoverHoverCollective, 0.f);
		constexpr float Delta = 0.2f;
		for (int32 Index = 0; Index < Diagonal.Num(); ++Index)
		{
			const FIntVector& Origin = Thrusters[Index].Origin;
			if (Origin.X == 0 || Origin.X == 5)
			{
				continue;   // forward pair and the mid pair sit this one out
			}
			const bool bFront = Origin.X == 8;
			const bool bLeftRail = Origin.Y == 0;
			Diagonal[Index].Trim = (bFront == bLeftRail) ? Delta : -Delta;
		}
		const FVector Couple = DeliveredTrimTorqueNm(Diagonal);
		FVector Axes[2];
		RoverNullAxes(Axes);
		TestTrue(FString::Printf(TEXT("a diagonal trim of %.2f makes %.0f N*m of yaw"), Delta, Couple.Z),
			FMath::Abs(Couple.Z) > 150.f);
		TestTrue(FString::Printf(TEXT("with no roll or pitch (%.1f, %.1f N*m)"), Couple.X, Couple.Y),
			FMath::Abs(Couple.X) < 5.f && FMath::Abs(Couple.Y) < 5.f);
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const FVector Probe = (Axis == 0) ? FVector::ForwardVector
				: ((Axis == 1) ? FVector::YAxisVector : FVector::UpVector);
			TestTrue(FString::Printf(TEXT("and no net force on axis %d (%.2f N)"), Axis,
					NetTrimForceAlongN(Diagonal, Probe)),
				FMath::Abs(NetTrimForceAlongN(Diagonal, Probe)) < 5.f);
		}

		// (c) AND IT MORE THAN COVERS THE HOLD COST, which is the whole point.
		const float HoldNm = RoverHullAngularDamping * RoverInertiaKgM2.Z
			* FMath::DegreesToRadians(RoverRateCeilingDegPerSec);
		TestTrue(FString::Printf(TEXT("%.0f N*m of yaw trim against a %.0f N*m hold cost at %.0f deg/s"),
				FMath::Abs(Couple.Z), HoldNm, RoverRateCeilingDegPerSec),
			FMath::Abs(Couple.Z) > HoldNm * 1.5f);

		// (d) The cost, stated: cos^2 of the cant, and nothing hidden.
		TestEqual(FString::Printf(TEXT("vertical lift is %.0f N of the %.0f N installed"),
				RoverVerticalLiftScaleN(), 6.f * ThrusterMaxThrustN),
			RoverVerticalLiftScaleN(), RoverLiftScaleN, 5.f);
		TestTrue(FString::Printf(TEXT("which costs %.1f percent of installed lift"),
				100.f * (1.f - RoverLiftScaleN / (6.f * ThrusterMaxThrustN))),
			RoverLiftScaleN < 6.f * ThrusterMaxThrustN
			&& RoverLiftScaleN > 6.f * ThrusterMaxThrustN * 0.97f);
	}

	// The lift group's centroid, which the spawner comment says lands on the
	// centre of mass. It nearly does - 27.0 mm aft - and that 27 mm is the
	// craft's dominant standing pitch moment: 485 N*m at the hover collective
	// and 578 N*m at the reserved ceiling, measured, which is exactly the sort
	// of thing that has to be a number rather than a claim. (Holding W subtracts
	// the forward pair's 270 N*m nose-up residual and leaves 241 N*m.) All of it
	// is cancelled by the trim - see StandingMomentIsCancelledByThrust - so the
	// figure that decides flyability is the residual, and the residual is zero.
	FVector LiftCentroid = FVector::ZeroVector;
	int32 LiftCount = 0;
	for (const FRoverThruster& Thruster : Thrusters)
	{
		if (Thruster.Origin.X != 0)
		{
			LiftCentroid += BlockCentreM({ Thruster.Origin, FIntVector(1, 1, 1), 45.f });
			++LiftCount;
		}
	}
	TestEqual(TEXT("six units lift"), LiftCount, 6);
	LiftCentroid /= FMath::Max(LiftCount, 1);
	TestTrue(FString::Printf(TEXT("the lift centroid sits %.0f mm from the centre of mass in X"),
			FMath::Abs(LiftCentroid.X - CentreM.X) * 1000.f),
		FMath::Abs(LiftCentroid.X - CentreM.X) < 0.05f);

	// The forward pair STRADDLES the centre of mass: one below, one above.
	TArray<float> ForwardArmZ;
	for (const FRoverThruster& Thruster : Thrusters)
	{
		if (Thruster.Origin.X == 0)
		{
			ForwardArmZ.Add(static_cast<float>(
				BlockCentreM({ Thruster.Origin, FIntVector(1, 1, 1), 45.f }).Z - CentreM.Z));
		}
	}
	TestEqual(TEXT("two units face forward"), ForwardArmZ.Num(), 2);
	if (ForwardArmZ.Num() == 2)
	{
		TestTrue(FString::Printf(TEXT("the forward pair straddles the centre of mass (%.3f m and %+.3f m)"),
				ForwardArmZ[0], ForwardArmZ[1]),
			ForwardArmZ[0] * ForwardArmZ[1] < 0.f);
	}

	// And the reserved ceiling is what it costs: TWR is the ceiling times the
	// installed lift, not the installed lift.
	TestEqual(TEXT("the pilot's ceiling is 0.90"), RoverCeiling, 0.9f, 1e-4f);
	const float CeilingTwr = RoverCeiling * RoverLiftScaleN / RoverWeightN;
	TestTrue(FString::Printf(TEXT("the reserved craft still climbs: TWR %.3f against %.3f installed, %.2f m/s2"),
			CeilingTwr, RoverLiftScaleN / RoverWeightN,
			RoverCeiling * RoverLiftScaleN / RoverMassKg - PlanetGravityMS2),
		CeilingTwr > 1.05f);
	return true;
}

/**
 * THE FIRST REGRESSION, pinned. Move.Z is a climb LEVEL, so releasing the key
 * means zero in, and zero in closes the valve. Every path that leaves the
 * target at zero - the descend key, a wheel back on the ground, an input
 * timeout, an unpiloted hull - therefore stops the climb, and there is no
 * setting left behind for the pilot to discover.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerThrustReleasedLiftKeyClosesTheValve, "Exoneer.Thrust.ReleasedLiftKeyClosesTheValve",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerThrustReleasedLiftKeyClosesTheValve::RunTest(const FString& Parameters)
{
	for (const float Fps : { 120.f, 60.f, 20.f, 10.f })
	{
		const float Dt = 1.f / Fps;
		float Valve = 0.f;
		const int32 StepsToFull = FMath::CeilToInt(1.f / (ValveSlewPerSec * Dt));
		for (int32 Step = 0; Step < StepsToFull; ++Step)
		{
			const float Previous = Valve;
			Valve = AdvanceCollective(Valve, 1.f, Dt, ValveSlewPerSec);
			TestTrue(FString::Printf(TEXT("held key opens the valve monotonically at %.0f fps"), Fps),
				Valve >= Previous - KINDA_SMALL_NUMBER);
			TestTrue(FString::Printf(TEXT("the valve never exceeds full at %.0f fps"), Fps), Valve <= 1.f + 1e-6f);
			TestTrue(FString::Printf(TEXT("the valve honours the slew rate at %.0f fps"), Fps),
				Valve - Previous <= ValveSlewPerSec * Dt + 1e-5f);
		}
		TestEqual(FString::Printf(TEXT("full travel takes 1/slew seconds at %.0f fps"), Fps), Valve, 1.f, 1e-4f);

		// THE PROPERTY: a zero target drives the commanded lift to exactly
		// zero, in the same time it took to open, at every frame rate.
		int32 Steps = 0;
		while (Valve > 0.f && Steps < 10000)
		{
			const float Previous = Valve;
			Valve = AdvanceCollective(Valve, 0.f, Dt, ValveSlewPerSec);
			TestTrue(FString::Printf(TEXT("a zero target closes the valve monotonically at %.0f fps"), Fps),
				Valve <= Previous + KINDA_SMALL_NUMBER);
			++Steps;
		}
		TestEqual(FString::Printf(TEXT("a zero target reaches EXACTLY zero at %.0f fps"), Fps), Valve, 0.f);
		TestTrue(FString::Printf(TEXT("closing takes about 1/slew seconds at %.0f fps (%.3f s)"), Fps, Steps * Dt),
			Steps * Dt <= 1.f / ValveSlewPerSec + 2.f * Dt);

		for (int32 Step = 0; Step < 600; ++Step)
		{
			Valve = AdvanceCollective(Valve, 0.f, Dt, ValveSlewPerSec);
		}
		TestEqual(FString::Printf(TEXT("a zeroed target keeps the valve shut at %.0f fps"), Fps), Valve, 0.f);
	}

	// From ANY setting, one frame of a zero target strictly reduces lift. This
	// is what the latched collective could not do: its zero meant hold.
	for (const float Setting : { 0.05f, 0.25f, 0.5f, 0.9f, 1.f })
	{
		const float Next = AdvanceCollective(Setting, 0.f, 1.f / 60.f, ValveSlewPerSec);
		TestTrue(FString::Printf(TEXT("a zero target reduces a %.2f setting (to %.4f)"), Setting, Next),
			Next < Setting);
	}
	TestTrue(TEXT("a held key raises the hover setting"),
		AdvanceCollective(RoverHoverCollective, RoverCeiling, 1.f / 60.f, ValveSlewPerSec) > RoverHoverCollective);
	TestEqual(TEXT("a level above 1 clamps"), AdvanceCollective(1.f, 5.f, 1.f, ValveSlewPerSec), 1.f);
	TestEqual(TEXT("a level below 0 clamps"), AdvanceCollective(0.f, -5.f, 1.f, ValveSlewPerSec), 0.f);
	return true;
}

/**
 * THE OTHER HALF OF THE SAME COMPLAINT. Making the valve purely binary stopped
 * the runaway climb and left the pilot with two states: climbing at 0.19 g or
 * falling at 1 g, on a craft whose visor showed a LIFT percentage he had no way
 * to choose. The governor is the missing middle.
 *
 * It is a throttle governor and nothing more: it moves the valve, inside the
 * valve's own travel and slew rate, and every newton it commands is a thruster
 * burning power. It carries a rate term and NO position term, so it holds zero
 * climb rather than an altitude, cannot wind up, and has no state the pilot
 * cannot read off the vertical-speed readout.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerThrustHoverGovernorHoldsAltitude, "Exoneer.Thrust.HoverGovernorHoldsAltitude",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerThrustHoverGovernorHoldsAltitude::RunTest(const FString& Parameters)
{
	// Level lift scale as the second argument in every call but the
	// underpowered one: the governor freezes only when the ATTITUDE took its
	// authority away, and runs to the stop when the craft simply cannot lift.
	const auto Governor = [](float LiftScaleN, float VerticalMS, float TargetMS, float Held)
	{
		return HoverCollective(RoverWeightN, LiftScaleN, RoverLiftScaleN, VerticalMS, TargetMS,
			HoverDampingAuthored, RoverCeiling, Held);
	};
	const float Hover = RoverHoverCollective;

	// The feed-forward is weight over vertical lift, so a level rover sits at
	// 0.763 - which is measured off the real, canted thruster set rather than
	// off a constant.
	TestEqual(FString::Printf(TEXT("level hover asks for %.3f of the valve"), Hover),
		Governor(RoverVerticalLiftScaleN(), 0.f, 0.f, Hover), Hover, 1e-3f);

	// A banked craft needs more valve, and is asked for it - up to the point
	// where the ceiling can no longer make weight at all. THE LIFT SCALE IS
	// DERIVED FROM A BANKED THRUSTER SET, not from cos(bank) times a constant,
	// so this is the number the router really computes.
	{
		const float Scale20 = RoverVerticalLiftScaleN(20.f);
		TestTrue(FString::Printf(TEXT("20 degrees of bank drops vertical lift to %.0f N"), Scale20),
			Scale20 < RoverLiftScaleN * 0.95f);
		TestTrue(TEXT("and a banked craft is asked for more valve"),
			Governor(Scale20, 0.f, 0.f, Hover) > Hover + 0.03f);
	}

	// Descending asks for more, climbing for less, and neither may pass the
	// reserved ceiling or go below shut.
	TestTrue(TEXT("descending opens the valve"),
		Governor(RoverLiftScaleN, -1.f, 0.f, Hover) > Hover);
	TestTrue(TEXT("climbing closes it"),
		Governor(RoverLiftScaleN, 1.f, 0.f, Hover) < Hover);
	TestEqual(TEXT("the governor never passes the reserved ceiling"),
		Governor(RoverLiftScaleN, -50.f, 0.f, Hover), RoverCeiling, 1e-4f);
	TestEqual(TEXT("and never goes below shut"),
		Governor(RoverLiftScaleN, 50.f, 0.f, Hover), 0.f);

	// B8 AND B13, THE ATTITUDE-TO-COLLECTIVE COUPLING, IN BOTH OF ITS FORMS.
	//
	// The first form kept the governor evaluating past the bank at which the
	// reserved ceiling can make weight: it pinned the valve at 0.90 and then,
	// the instant cos(bank) reached zero, commanded it fully SHUT and slammed
	// it open again on the way out - a 0.9 to 0 to 0.9 excursion caused by an
	// attitude change alone. The answer to that was to FREEZE the valve.
	//
	// The second form was the freeze itself, taken too far. AlongWorldUp was
	// clamped at zero, so from 90 degrees onward the freeze branch was
	// permanently true and held the valve at whatever the pilot had - which at
	// that boundary is always the ceiling. Inverted, that is 21.4 kN of lift
	// pointing at the ground: 2.1 g downward ON TOP of weight, and strictly
	// worse than the shut valve it replaced, which was 1 g.
	//
	// The invariant that settles both is NOT about the valve, it is about the
	// FORCE: at no attitude may the governor command a valve whose net
	// world-vertical force is negative. Below 90 degrees that permits the
	// freeze; past 90 it forbids it and there is nothing left to freeze at.
	{
		const float LimitScale = RoverWeightN / RoverCeiling;
		float BankLimitDeg = 0.f;
		for (float Deg = 0.f; Deg <= 90.f; Deg += 0.1f)
		{
			if (RoverVerticalLiftScaleN(Deg) < LimitScale)
			{
				BankLimitDeg = Deg;
				break;
			}
		}
		TestTrue(FString::Printf(TEXT("the ceiling stops holding weight at %.1f degrees of bank"), BankLimitDeg),
			BankLimitDeg > 25.f && BankLimitDeg < 40.f);

		// THE FREEZE BAND, 32 to 90 degrees: frozen, never shut, and still
		// making lift upward. This is the band where "leave the valve alone and
		// let it sink" is the honest answer.
		for (const float Deg : { 35.f, 60.f, 89.f })
		{
			const float Held = 0.72f;
			const float Frozen = Governor(RoverVerticalLiftScaleN(Deg), 0.f, 0.f, Held);
			TestEqual(FString::Printf(TEXT("at %.0f degrees the valve is frozen where the pilot left it"), Deg),
				Frozen, Held, 1e-4f);
			TestTrue(FString::Printf(TEXT("and %.0f degrees still makes %.0f N of upward force at that valve"),
					Deg, RoverVerticalForceN(Frozen, Deg)),
				RoverVerticalForceN(Frozen, Deg) > 0.f);
		}
		// Continuous at the LOWER boundary: the governed value at the limit
		// angle is the ceiling, so the freeze does not step.
		TestEqual(FString::Printf(TEXT("the freeze is continuous at %.1f degrees"), BankLimitDeg),
			Governor(RoverVerticalLiftScaleN(BankLimitDeg - 0.2f), 0.f, 0.f, RoverCeiling),
			RoverCeiling, 2e-2f);

		// PAST 90 DEGREES the valve is SHUT, deliberately, and the boundary is
		// continuous in the only quantity that matters: at 90 degrees the craft
		// makes no net vertical force at ANY valve setting, so the 0.90 to 0
		// step in the valve is a 0 N to 0 N step in force.
		TestTrue(FString::Printf(TEXT("at 90 degrees the craft makes %.0f N of vertical force at full valve"),
				RoverVerticalForceN(RoverCeiling, 90.f)),
			FMath::Abs(RoverVerticalForceN(RoverCeiling, 90.f)) < 50.f);
		for (const float Deg : { 95.f, 120.f, 180.f })
		{
			const float Commanded = Governor(RoverVerticalLiftScaleN(Deg), 0.f, 0.f, RoverCeiling);
			const float WouldHaveBeen = RoverVerticalForceN(RoverCeiling, Deg);
			TestEqual(FString::Printf(TEXT("at %.0f degrees the valve is commanded shut, not frozen"), Deg),
				Commanded, 0.f);
			TestTrue(FString::Printf(
					TEXT("which is worth %.0f N: the frozen ceiling would have pushed %.1f kN DOWNWARD (%.2f g on top of weight)"),
					RoverVerticalForceN(Commanded, Deg), WouldHaveBeen / 1000.f,
					-WouldHaveBeen / RoverWeightN),
				WouldHaveBeen < -1000.f && FMath::Abs(RoverVerticalForceN(Commanded, Deg)) < 1.f);
		}

		// Roll all the way over and back with NO key held, one degree at a
		// time. The valve may not move faster than the slew rate, may never
		// exceed the ceiling, and may NEVER be left at a setting whose net
		// vertical force is negative.
		float Valve = Hover;
		float Highest = Valve;
		float WorstStep = 0.f;
		float WorstDownForce = 0.f;
		float ShutBelow90Deg = -1.f;
		constexpr float Dt = 1.f / 60.f;
		for (int32 Step = 0; Step <= 360; ++Step)
		{
			const float Deg = (Step <= 180) ? Step * 1.f : (360 - Step) * 1.f;
			const float Target = Governor(RoverVerticalLiftScaleN(Deg), 0.f, 0.f, Valve);
			const float Previous = Valve;
			Valve = AdvanceCollective(Valve, Target, Dt, ValveSlewPerSec);
			WorstStep = FMath::Max(WorstStep, FMath::Abs(Valve - Previous));
			Highest = FMath::Max(Highest, Valve);
			WorstDownForce = FMath::Min(WorstDownForce, RoverVerticalForceN(Valve, Deg));
			// Only the OUTBOUND leg: on the way back the valve is climbing off
			// its stop again, so it is legitimately near zero just under 90.
			if (Step <= 180 && Deg < 85.f && Valve < 0.05f && ShutBelow90Deg < 0.f)
			{
				ShutBelow90Deg = Deg;
			}
		}
		// A REAL VALVE CANNOT SLAM. What is left of the powered dive is the
		// 0.45 s the valve takes to slew shut, which is 27 degrees of roll at
		// one degree per frame and worth 2.3 kN downward at its worst - 0.12 g
		// on top of weight, against the 21.4 kN (1.18 g) the frozen ceiling
		// held all the way to the ground.
		TestTrue(FString::Printf(
				TEXT("through a full roll the worst downward force is %+.0f N (%.2f g) and the valve never passes the ceiling (%.3f)"),
				WorstDownForce, -WorstDownForce / RoverWeightN, Highest),
			WorstDownForce > -0.2f * RoverWeightN && Highest <= RoverCeiling + 1e-4f);
		TestTrue(FString::Printf(TEXT("and it is never shut inside the freeze band (first shut at %.0f degrees)"),
				ShutBelow90Deg),
			ShutBelow90Deg < 0.f);
		TestTrue(FString::Printf(TEXT("and never jumps: worst single-frame move %.4f"), WorstStep),
			WorstStep <= ValveSlewPerSec * Dt + 1e-4f);

		// The two machine states the visor has to keep apart. PINNED is
		// "sinking with the valve where you left it"; INVERT is "falling with
		// the valve shut because the lift points at the ground", and the
		// pilot's way out is different - roll back, do not add throttle.
		TestTrue(TEXT("banked out of authority reads PINNED, not HOVER"),
			IsHoverGovernorPinned(RoverWeightN, RoverVerticalLiftScaleN(60.f), RoverLiftScaleN, RoverCeiling));
		TestFalse(TEXT("while a level craft is not pinned"),
			IsHoverGovernorPinned(RoverWeightN, RoverVerticalLiftScaleN(0.f), RoverLiftScaleN, RoverCeiling));
		TestFalse(TEXT("and an inverted craft is not pinned either - it is inverted"),
			IsHoverGovernorPinned(RoverWeightN, RoverVerticalLiftScaleN(180.f), RoverLiftScaleN, RoverCeiling));
		TestTrue(TEXT("inverted reads INVERT"), IsLiftInverted(RoverVerticalLiftScaleN(180.f)));
		TestFalse(TEXT("level does not"), IsLiftInverted(RoverVerticalLiftScaleN(0.f)));
		TestFalse(TEXT("nor does 60 degrees of bank"), IsLiftInverted(RoverVerticalLiftScaleN(60.f)));
	}

	// B9. THE DESCEND KEY IS A RATE COMMAND, and the asymmetry it used to have
	// is gone. Four seconds of held Ctrl, then release, both ways.
	{
		constexpr float Dt = 1.f / 120.f;
		const auto Fly = [&](bool bKill, float HoldSeconds, float& OutSpeed, float& OutDrop,
			float& OutArrestSeconds, float& OutTotalDrop)
		{
			float Valve = Hover;
			float SpeedMS = 0.f;
			float Height = 0.f;
			for (int32 Step = 0; Step < FMath::RoundToInt(HoldSeconds / Dt); ++Step)
			{
				const float Target = bKill ? 0.f : Governor(RoverLiftScaleN, SpeedMS, -DescentRateAuthored, Valve);
				Valve = AdvanceCollective(Valve, Target, Dt, ValveSlewPerSec);
				SpeedMS += (RoverLiftScaleN * Valve / RoverMassKg - PlanetGravityMS2) * Dt;
				Height += SpeedMS * Dt;
			}
			OutSpeed = SpeedMS;
			OutDrop = Height;
			int32 Steps = 0;
			while (SpeedMS < -0.05f && Steps < 60 * 120)
			{
				Valve = AdvanceCollective(Valve, Governor(RoverLiftScaleN, SpeedMS, 0.f, Valve), Dt, ValveSlewPerSec);
				SpeedMS += (RoverLiftScaleN * Valve / RoverMassKg - PlanetGravityMS2) * Dt;
				Height += SpeedMS * Dt;
				++Steps;
			}
			OutArrestSeconds = Steps * Dt;
			OutTotalDrop = Height;
		};

		float KillSpeed = 0.f, KillDrop = 0.f, KillArrest = 0.f, KillTotal = 0.f;
		Fly(true, 4.f, KillSpeed, KillDrop, KillArrest, KillTotal);
		float GovSpeed = 0.f, GovDrop = 0.f, GovArrest = 0.f, GovTotal = 0.f;
		Fly(false, 4.f, GovSpeed, GovDrop, GovArrest, GovTotal);

		AddInfo(FString::Printf(
			TEXT("4 s of Ctrl: valve-kill reaches %.1f m/s and %.0f m, needing %.1f s and %.0f m to arrest; ")
			TEXT("governed reaches %.2f m/s and %.1f m, needing %.1f s and %.1f m"),
			KillSpeed, -KillDrop, KillArrest, -KillTotal, GovSpeed, -GovDrop, GovArrest, -GovTotal));

		TestTrue(FString::Printf(TEXT("the old valve kill passed the 8 m/s damage threshold (%.1f m/s)"), KillSpeed),
			KillSpeed < -8.f);
		TestTrue(FString::Printf(TEXT("the governed descent holds %.2f m/s, inside the authored %.1f"),
				GovSpeed, DescentRateAuthored),
			GovSpeed > -DescentRateAuthored * 1.2f && GovSpeed < -DescentRateAuthored * 0.8f);
		TestTrue(FString::Printf(TEXT("and never reaches landing-damage speed (%.2f against 8 m/s)"), GovSpeed),
			GovSpeed > -8.f);
		TestTrue(FString::Printf(TEXT("release levels off in %.1f s and %.1f m"), GovArrest, -GovTotal),
			GovArrest < 8.f && -GovTotal < 25.f);
		// The 5:1 asymmetry: descent authority against arrest authority.
		const float ArrestMS2 = (RoverCeiling * RoverLiftScaleN - RoverWeightN) / RoverMassKg;
		TestTrue(FString::Printf(TEXT("descent is now bounded at %.1f m/s against %.2f m/s2 of arrest, not 1 g against %.2f"),
				DescentRateAuthored, ArrestMS2, ArrestMS2),
			DescentRateAuthored / FMath::Max(ArrestMS2, 0.01f) < 2.f);
		// And the valve stays OFF ITS BOTTOM STOP, which is what keeps the
		// trim path alive: Ctrl+W used to be the one state with no authority.
		float DescendValve = Hover;
		for (int32 Step = 0; Step < 600; ++Step)
		{
			DescendValve = AdvanceCollective(DescendValve,
				Governor(RoverLiftScaleN, -DescentRateAuthored, -DescentRateAuthored, DescendValve),
				1.f / 60.f, ValveSlewPerSec);
		}
		TestTrue(FString::Printf(TEXT("a governed descent leaves the valve at %.3f, not shut"), DescendValve),
			DescendValve > 0.3f);
		TArray<FTrimEffector> Descending = MakeRoverThrusters(DescendValve, RoverCeiling);
		const FVector Standing = StandingMomentNm(Descending);
		FVector Axes[2];
		RoverNullAxes(Axes);
		AllocateForceNeutralTrim(Descending, -Standing, Axes, 2);
		const FVector Residual = Standing + DeliveredTrimTorqueNm(Descending);
		TestTrue(FString::Printf(TEXT("so Ctrl+W still trims: %.0f N*m down to %.1f N*m"),
				Standing.GetAbsMax(), Residual.GetAbsMax()),
			Residual.GetAbsMax() < 5.f);
	}

	// THE CLOSED LOOP, valve slew included. Dropped at 3 m/s, the craft has to
	// arrest and hold, and it must not overshoot into a climb it did not ask
	// for. Time constant is mass / (lift scale x gain) = 0.77 s.
	{
		constexpr float Dt = 1.f / 60.f;
		float Valve = RoverHoverCollective;
		float VerticalSpeedMS = -3.f;
		float AltitudeM = 0.f;
		float SettledAt = -1.f;
		float PeakClimb = 0.f;
		for (int32 Step = 0; Step < 3600; ++Step)   // 60 s
		{
			const float Target = HoverCollective(RoverWeightN, RoverLiftScaleN, RoverLiftScaleN,
				VerticalSpeedMS, 0.f, HoverDampingAuthored, RoverCeiling, Valve);
			Valve = AdvanceCollective(Valve, Target, Dt, ValveSlewPerSec);
			VerticalSpeedMS += (RoverLiftScaleN * Valve / RoverMassKg - PlanetGravityMS2) * Dt;
			AltitudeM += VerticalSpeedMS * Dt;
			PeakClimb = FMath::Max(PeakClimb, VerticalSpeedMS);
			if (SettledAt < 0.f && FMath::Abs(VerticalSpeedMS) < 0.15f)
			{
				SettledAt = Step * Dt;
			}
		}
		TestTrue(FString::Printf(TEXT("a 3 m/s drop is arrested in %.2f s"), SettledAt),
			SettledAt > 0.f && SettledAt < 5.f);
		TestTrue(FString::Printf(TEXT("without overshooting into a climb (peak %+.2f m/s)"), PeakClimb),
			PeakClimb < 0.5f);
		TestTrue(FString::Printf(TEXT("and holds: %+.2f m/s after a minute"), VerticalSpeedMS),
			FMath::Abs(VerticalSpeedMS) < 0.05f);
		TestTrue(FString::Printf(TEXT("total sag over the recovery is %.2f m"), AltitudeM),
			AltitudeM > -4.f && AltitudeM < 0.5f);
	}

	// NO MAGIC. A craft that cannot lift its own weight runs the valve to the
	// stop and keeps sinking; the governor has no other actuator to reach for.
	{
		const float WeakLiftN = RoverWeightN * 0.8f;
		constexpr float Dt = 1.f / 60.f;
		float Valve = 0.f;
		float VerticalSpeedMS = 0.f;
		for (int32 Step = 0; Step < 600; ++Step)
		{
			Valve = AdvanceCollective(Valve,
				HoverCollective(RoverWeightN, WeakLiftN, WeakLiftN, VerticalSpeedMS, 0.f,
					HoverDampingAuthored, RoverCeiling, Valve),
				Dt, ValveSlewPerSec);
			VerticalSpeedMS += (WeakLiftN * Valve / RoverMassKg - PlanetGravityMS2) * Dt;
		}
		// Underpowered is NOT the same state as banked-past-holding-weight and
		// must not freeze: no attitude would help, so the valve goes to the
		// stop and the craft sinks anyway.
		TestEqual(TEXT("an underpowered craft pins the valve at the ceiling"), Valve, RoverCeiling, 1e-3f);
		TestFalse(TEXT("and is not reported as PINNED, which means banked out of authority"),
			IsHoverGovernorPinned(RoverWeightN, WeakLiftN, WeakLiftN, RoverCeiling));
		TestTrue(FString::Printf(TEXT("and still sinks, at %.2f m/s"), VerticalSpeedMS), VerticalSpeedMS < -1.f);
	}

	// The governor is not a latch: a zero target still closes the valve from
	// wherever the governor left it, which is what the descend key, touching
	// down, an input timeout and leaving the seat all do.
	{
		float Valve = RoverHoverCollective;
		for (int32 Step = 0; Step < 120; ++Step)
		{
			Valve = AdvanceCollective(Valve, 0.f, 1.f / 60.f, ValveSlewPerSec);
		}
		TestEqual(TEXT("a zero target closes a governed valve completely"), Valve, 0.f);
	}
	return true;
}

/**
 * B2, pinned, and the bound corrected. A valve may be biased DOWN as far as it
 * can close and UP as far as it can open - which is simply its own travel, and
 * is asymmetric at every setting off the middle. The single symmetric bound
 * that used to be here was pessimistic by half at hover and claimed a shut
 * valve could never open, which is not true of any valve.
 *
 * What remains true is that a valve at a STOP has no travel in that direction,
 * so the reserve buys the top of the lever back - and full collective is
 * exactly where a climbing pilot who needs to shed a saturated rotor sits.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerThrustReserveKeepsAuthorityAtFullCollective, "Exoneer.Thrust.ReserveKeepsAuthorityAtFullCollective",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerThrustReserveKeepsAuthorityAtFullCollective::RunTest(const FString& Parameters)
{
	// THE DEFECT: with the pilot able to reach the stop, there is no travel
	// left in the direction that matters.
	{
		FTrimEffector AtStop;
		AtStop.TrimFraction = TrimFractionAuthored;
		AtStop.BaseThrottle = 1.f;
		TestEqual(TEXT("a valve at the stop cannot open at all"), TrimBoundMax(AtStop), 0.f);
		TestEqual(TEXT("though it can still close"), TrimBoundMin(AtStop), -TrimFractionAuthored, 1e-4f);
		TestEqual(TEXT("so its symmetric authority is zero"), TrimBound(AtStop), 0.f);

		// AND THE OTHER END, corrected: a shut valve CAN open. Saying it could
		// not was the pessimism that made a descending craft authorityless.
		AtStop.BaseThrottle = 0.f;
		TestEqual(TEXT("a shut valve cannot close further"), TrimBoundMin(AtStop), 0.f);
		TestEqual(TEXT("but it can open"), TrimBoundMax(AtStop), TrimFractionAuthored, 1e-4f);
	}

	// THE FIX: held to the reserved ceiling, every unit keeps the reserve on
	// the way up and its whole authored fraction on the way down.
	{
		FTrimEffector Reserved;
		Reserved.TrimFraction = TrimFractionAuthored;
		Reserved.BaseThrottle = RoverCeiling;
		TestEqual(FString::Printf(TEXT("a valve at the ceiling keeps %.2f of travel upward"), TrimBoundMax(Reserved)),
			TrimBoundMax(Reserved), ReserveFractionAuthored, 1e-4f);
		TestEqual(TEXT("and its full authored fraction downward"),
			TrimBoundMin(Reserved), -TrimFractionAuthored, 1e-4f);
	}

	// The slew can carry a bias past a bound that shrank while the collective
	// moved. ClampTrimsToBounds is what stops that reaching the commit.
	{
		TArray<FTrimEffector> Effectors = MakeRoverThrusters(RoverCeiling, 0.f);
		for (FTrimEffector& Effector : Effectors)
		{
			Effector.Trim = 0.34f;   // legal a frame ago at a lower collective
		}
		ClampTrimsToBounds(Effectors);
		for (const FTrimEffector& Effector : Effectors)
		{
			TestTrue(FString::Printf(TEXT("a stale bias is clamped to %.3f..%.3f (got %.3f)"),
					TrimBoundMin(Effector), TrimBoundMax(Effector), Effector.Trim),
				Effector.Trim <= TrimBoundMax(Effector) + 1e-6f
				&& Effector.Trim >= TrimBoundMin(Effector) - 1e-6f);
		}
	}

	// And it is enough on the real craft: at FULL collective, with the forward
	// key held, the allocator cancels the whole standing moment.
	{
		TArray<FTrimEffector> Effectors = MakeRoverThrusters(RoverCeiling, RoverCeiling);
		const FVector Standing = StandingMomentNm(Effectors);
		TestTrue(FString::Printf(TEXT("there is a standing moment to cancel: (%.0f, %.0f, %.0f) N*m"),
				Standing.X, Standing.Y, Standing.Z),
			Standing.GetAbsMax() > 100.f);
		FVector Axes[2];
		RoverNullAxes(Axes);
		AllocateForceNeutralTrim(Effectors, -Standing, Axes, 2);
		const FVector Residual = Standing + DeliveredTrimTorqueNm(Effectors);
		TestTrue(FString::Printf(TEXT("and at full collective it is cancelled to (%.2f, %.2f, %.2f) N*m"),
				Residual.X, Residual.Y, Residual.Z),
			Residual.GetAbsMax() < 1.f);
	}
	return true;
}

/**
 * B1, pinned, with the defect measured beside the fix. A thrust layout whose
 * net moment about the centre of mass is non-zero makes a moment that NEVER
 * ENDS, and a reaction wheel can only hold one by winding its rotors - so the
 * momentum store fills at the moment's own magnitude per second and the axis
 * dies on a stopwatch. Thrust cancels thrust for free.
 *
 * Two things had to change and both are checked here: the shipped rover's
 * forward pair now straddles the centre of mass, and the router cancels
 * whatever standing moment ANY build makes, measured about the real centre of
 * mass, so a craft the player throws together is trimmed too.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerThrustStandingMomentIsCancelledByThrust, "Exoneer.Thrust.StandingMomentIsCancelledByThrust",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerThrustStandingMomentIsCancelledByThrust::RunTest(const FString& Parameters)
{
	// THE DEFECT. The old pair, both below the centre of mass, at full forward.
	{
		TArray<FTrimEffector> Previous = MakePreviousRoverThrusters(0.f, 1.f);
		const float PitchNm = FMath::Abs(static_cast<float>(StandingMomentNm(Previous).Y));
		const float SecondsToStop = RoverCapacityNms / FMath::Max(PitchNm, 1.f);
		TestTrue(FString::Printf(TEXT("the previous layout made %.0f N*m of standing pitch, %.1f s of rotor"),
				PitchNm, SecondsToStop),
			PitchNm > 1000.f && SecondsToStop < 2.f);
	}
	// THE FIX, geometry half: the straddled pair, same thrust, same mass.
	{
		TArray<FTrimEffector> Shipped = MakeRoverThrusters(0.f, 1.f);
		const float PitchNm = FMath::Abs(static_cast<float>(StandingMomentNm(Shipped).Y));
		const float SecondsToStop = RoverCapacityNms / FMath::Max(PitchNm, 1.f);
		TestTrue(FString::Printf(TEXT("the shipped layout makes %.0f N*m, %.1f s of rotor even untrimmed"),
				PitchNm, SecondsToStop),
			PitchNm < 350.f && SecondsToStop > 4.f);
	}

	// THE FIX, controller half. Across the whole band the pilot flies, with and
	// without the forward key, AND AT THE ATTITUDES HE FLIES IT AT, the
	// allocator leaves the rotors NOTHING to hold.
	//
	// The bank and pitch cases are new. Every previous version of this fixture
	// was level-only, so the governor, the trim and the lift scale were never
	// evaluated on a banked craft at all - and the banked craft is where the
	// pilot's complaint came from.
	for (const float Collective : { 0.05f, 0.2f, 0.4f, RoverHoverCollective, 0.85f, RoverCeiling })
	{
		for (const float Forward : { 0.f, 0.5f, RoverCeiling })
		{
			for (const FVector2D Attitude : { FVector2D(0.f, 0.f), FVector2D(25.f, 0.f),
				FVector2D(0.f, 20.f), FVector2D(-40.f, 15.f) })
			{
				const float BankDeg = static_cast<float>(Attitude.X);
				const float PitchDeg = static_cast<float>(Attitude.Y);
				TArray<FTrimEffector> Effectors = MakeRoverThrusters(Collective, Forward, 1.f, BankDeg, PitchDeg);
				const float LiftBefore = TotalLiftN(Effectors);
				const FVector Standing = StandingMomentNm(Effectors);
				FVector Axes[2];
				RoverNullAxes(Axes, BankDeg, PitchDeg);
				AllocateForceNeutralTrim(Effectors, -Standing, Axes, 2);
				const FVector Residual = Standing + DeliveredTrimTorqueNm(Effectors);
				TestTrue(FString::Printf(
						TEXT("collective %.2f forward %.2f bank %.0f pitch %.0f: standing %.0f N*m cancelled to %.2f N*m"),
						Collective, Forward, BankDeg, PitchDeg, Standing.GetAbsMax(), Residual.GetAbsMax()),
					Residual.GetAbsMax() < 1.f);
				TestEqual(FString::Printf(TEXT("and the altitude does not move (collective %.2f, bank %.0f)"),
						Collective, BankDeg),
					TotalLiftN(Effectors), LiftBefore, 3.f);
				// NOR THE GROUND TRACK ALONG THE CRAFT'S FORWARD AXIS. A bias
				// on a forward-facing unit has no lift component at all, so the
				// lift-only null let it change the ground track by up to 400 N.
				TestTrue(FString::Printf(TEXT("and neither does the forward axis (%.2f N)"),
						NetTrimForceAlongN(Effectors, Axes[1])),
					FMath::Abs(NetTrimForceAlongN(Effectors, Axes[1])) < 5.f);
				for (const FTrimEffector& Effector : Effectors)
				{
					TestTrue(TEXT("no valve is biased past its authored authority"),
						Effector.Trim <= TrimBoundMax(Effector) + 1e-5f
						&& Effector.Trim >= TrimBoundMin(Effector) - 1e-5f);
				}
			}
		}
	}

	// THE HONEST LIMIT. A shut lift valve makes no thrust and therefore no
	// moment, so there is no authority there and the rotors do pay. The trim
	// must not make it WORSE than doing nothing - the backtracking safeguard -
	// and the visor reports the countdown.
	{
		TArray<FTrimEffector> Effectors = MakeRoverThrusters(0.f, RoverCeiling);
		const FVector Standing = StandingMomentNm(Effectors);
		FVector Axes[2];
		RoverNullAxes(Axes);
		AllocateForceNeutralTrim(Effectors, -Standing, Axes, 2);
		const FVector Residual = Standing + DeliveredTrimTorqueNm(Effectors);
		TestTrue(FString::Printf(TEXT("with the valve shut the residual is %.0f N*m, no worse than the %.0f N*m untrimmed"),
				Residual.GetAbsMax(), Standing.GetAbsMax()),
			Residual.GetAbsMax() <= Standing.GetAbsMax() + 1.f);
		// AND IT IS RECOVERABLE, which the previous version stopped short of
		// checking. The state only exists while the pilot holds the descend key
		// with the valve at the stop; the governed descent keeps the valve off
		// that stop, so opening the collective at all restores the authority.
		const float ShutResidual = Residual.GetAbsMax();
		TArray<FTrimEffector> Recovered = MakeRoverThrusters(0.2f, RoverCeiling);
		const FVector RecoveredStanding = StandingMomentNm(Recovered);
		AllocateForceNeutralTrim(Recovered, -RecoveredStanding, Axes, 2);
		const float RecoveredResidual = (RecoveredStanding + DeliveredTrimTorqueNm(Recovered)).GetAbsMax();
		TestTrue(FString::Printf(TEXT("a fifth of collective recovers it: %.0f N*m down to %.1f N*m"),
				ShutResidual, RecoveredResidual),
			RecoveredResidual < 5.f);
		TestTrue(FString::Printf(TEXT("and the visor reports the shut state as %.1f s of pitch"),
				RoverCapacityNms / FMath::Max(ShutResidual, 1.f)),
			ShutResidual > 1.f);
	}

	// ARBITRARY GEOMETRY. The property belongs to the allocator, not to a lucky
	// layout: on 300 random thrust arrangements the trim must be lift-neutral,
	// inside every bound, and never leave the craft worse off than untrimmed.
	{
		FRandomStream Random(20260904);
		int32 MeaningfullyReduced = 0;
		int32 ForwardAxisDropped = 0;
		float WorstForwardLeakFraction = 0.f;
		for (int32 Trial = 0; Trial < 300; ++Trial)
		{
			TArray<FTrimEffector> Effectors;
			const int32 Count = Random.RandRange(2, 12);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				FTrimEffector Effector;
				const FVector Arm = Random.VRand() * Random.FRandRange(0.1f, 3.f);
				const FVector Dir = Random.VRand();
				const float ThrustN = Random.FRandRange(500.f, 8000.f);
				Effector.MomentPerUnitThrottle = FVector::CrossProduct(Arm, Dir * ThrustN);
				Effector.ForcePerUnitThrottle = Dir * ThrustN;
				Effector.LiftPerUnitThrottle = ThrustN * FMath::Max(0.f, static_cast<float>(Dir.Z));
				Effector.BaseThrottle = Random.FRandRange(0.f, RoverCeiling);
				Effector.TrimFraction = Random.FRandRange(0.f, 0.6f);
				Effectors.Add(Effector);
			}
			const float LiftBefore = TotalVerticalForceN(Effectors);
			const FVector Standing = StandingMomentNm(Effectors);
			const FVector RandomAxes[2] = { FVector::UpVector, FVector::ForwardVector };
			AllocateForceNeutralTrim(Effectors, -Standing, RandomAxes, 2);
			const FVector Residual = Standing + DeliveredTrimTorqueNm(Effectors);

			TestTrue(FString::Printf(TEXT("trial %d: trim never makes the standing moment worse (%.1f from %.1f N*m)"),
					Trial, Residual.Size(), Standing.Size()),
				Residual.Size() <= Standing.Size() + 1e-3 * FMath::Max(1.0, Standing.Size()));
			// FORCE NEUTRALITY ON BOTH HELD AXES, measured on the real force
			// vector. The previous version checked the clamped lift row only,
			// which is how a 400 N uncommanded forward force survived three
			// thousand random layouts.
			const float Scale = FMath::Max(1.f, TrimForceScaleN(Effectors));
			const float VerticalLeak = NetTrimForceAlongN(Effectors, FVector::UpVector);
			const float ForwardLeak = NetTrimForceAlongN(Effectors, FVector::ForwardVector);
			TestTrue(FString::Printf(TEXT("trial %d leaks %.4f N vertically against a %.0f N force scale"),
					Trial, VerticalLeak, Scale),
				FMath::Abs(VerticalLeak) <= Scale * 1e-3f);
			TestEqual(FString::Printf(TEXT("trial %d holds its total vertical force"), Trial),
				TotalVerticalForceN(Effectors), LiftBefore, FMath::Max(1.f, Scale * 1e-3f));
			// THE FORWARD AXIS IS THE ONE THE ALLOCATOR MAY DROP, and on
			// arbitrary geometry it sometimes must: some layouts cannot make
			// the moment at all without a little push. Dropping it is the
			// designed degradation - a moment with a small side force beats no
			// moment, and lift is never dropped - so what is measured is HOW
			// OFTEN, not never.
			if (FMath::Abs(ForwardLeak) > Scale * 1e-3f)
			{
				++ForwardAxisDropped;
				WorstForwardLeakFraction = FMath::Max(WorstForwardLeakFraction,
					FMath::Abs(ForwardLeak) / Scale);
			}
			for (const FTrimEffector& Effector : Effectors)
			{
				TestTrue(FString::Printf(TEXT("trial %d respects the trim bound"), Trial),
					Effector.Trim <= TrimBoundMax(Effector) + 1e-4f
					&& Effector.Trim >= TrimBoundMin(Effector) - 1e-4f);
			}
			if (Residual.Size() < Standing.Size() * 0.9f)
			{
				++MeaningfullyReduced;
			}
		}
		// Most random layouts are NOT fully trimmable and should not be: a
		// thrust arrangement bolted together at random has valves near their
		// stops and moments that do not span the axes it needs. What must hold
		// is that the trim helps on most of them and hurts on none - measured
		// at 83 percent reduced by more than a tenth, and zero made worse,
		// over three thousand layouts.
		TestTrue(FString::Printf(TEXT("the trim helps on most random layouts (%d of 300 reduced by over a tenth)"),
				MeaningfullyReduced),
			MeaningfullyReduced > 120);
		AddInfo(FString::Printf(
			TEXT("300 arbitrary layouts: vertical force held on every one; the forward axis was dropped on ")
			TEXT("%d of them, worst leak %.1f%% of the craft's own force scale"),
			ForwardAxisDropped, WorstForwardLeakFraction * 100.f));
		TestTrue(FString::Printf(TEXT("the forward axis holds on all but %d of 300 layouts"), ForwardAxisDropped),
			ForwardAxisDropped < 15);
		TestTrue(FString::Printf(TEXT("and where it is dropped the leak stays under a tenth of the force scale (%.1f%%)"),
				WorstForwardLeakFraction * 100.f),
			WorstForwardLeakFraction < 0.10f);
	}
	return true;
}

/**
 * AN ATTITUDE TRANSIENT DOES NOT MOVE THE VALVE, AND NOTHING MOVES THE
 * ALTITUDE. Those are two separate properties and this test pins both,
 * because the second one is the invariant the pilot feels and the first is
 * only how it is achieved.
 *
 *   1. A TRANSIENT is the rotors' job. The trim request sees the attitude
 *      command only through a first-order lag, so at the authored 1.5 s a
 *      quarter-second stick input passes about 15 percent of itself and a
 *      one-frame spike passes 1 percent. Feed the same craft wildly different
 *      INSTANTANEOUS attitude commands and the valves come out identical.
 *   2. A SUSTAINED command does move the valves, deliberately - holding a rate
 *      against hull damping costs D*I*w every second for ever and a rotor
 *      paying it dies on a stopwatch - but it moves them FORCE-NEUTRALLY, so
 *      total lift and the forward ground track are unchanged to the newton.
 *
 * A previous pass took attitude torque from differential lift FIRST and gave
 * the triad the residual, which made every attitude correction a lift change
 * and made the attitude loop's authority a hidden function of the throttle
 * setting: zero at both ends of the travel, peak in the middle. That is still
 * excluded - the trim never sees the rate error, only its own lag of it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerThrustAttitudeDoesNotMoveTheValve, "Exoneer.Thrust.AttitudeDoesNotMoveTheValve",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerThrustAttitudeDoesNotMoveTheValve::RunTest(const FString& Parameters)
{
	constexpr float Dt = 1.f / 60.f;
	const FVector StoreInsideEnvelope(300.f, -500.f, 200.f);
	const TArray<FVector> AttitudeCommands =
	{
		FVector::ZeroVector,
		FVector(RoverRatedTorqueNm, 0.f, 0.f),
		FVector(0.f, -RoverRatedTorqueNm, 0.f),
		FVector(-RoverRatedTorqueNm, RoverRatedTorqueNm, -RoverRatedTorqueNm),
	};

	TArray<TArray<FTrimEffector>> Runs;
	for (int32 Case = 0; Case < AttitudeCommands.Num(); ++Case)
	{
		Runs.Add(MakeRoverThrusters(RoverHoverCollective, 0.6f));
	}
	const float LiftBefore = TotalLiftN(Runs[0]);
	TestTrue(TEXT("the craft is actually producing lift to lose"), LiftBefore > 1000.f);

	for (int32 Step = 0; Step < 120; ++Step)
	{
		for (int32 Case = 0; Case < AttitudeCommands.Num(); ++Case)
		{
			// No sustained term: this is the TRANSIENT property.
			RouteTrimFrame(Runs[Case], StoreInsideEnvelope, AttitudeCommands[Case], Dt);
		}
		for (int32 Case = 1; Case < AttitudeCommands.Num(); ++Case)
		{
			for (int32 Index = 0; Index < Runs[Case].Num(); ++Index)
			{
				TestEqual(TEXT("the instantaneous attitude command cannot move a valve"),
					Runs[Case][Index].Trim, Runs[0][Index].Trim, 1e-6f);
			}
			TestEqual(TEXT("and so total lift is identical too"),
				TotalLiftN(Runs[Case]), TotalLiftN(Runs[0]), 1e-2f);
		}
		TestEqual(TEXT("total lift is unchanged from the pilot's setting"),
			TotalLiftN(Runs[0]), LiftBefore, 3.f);
	}

	// THE LAG. A quarter-second stick input is a transient by construction, and
	// the numbers say by how much: 1 - exp(-t/T).
	{
		constexpr float Spike = 1000.f;
		FVector Sustained = FVector::ZeroVector;
		for (int32 Step = 0; Step < FMath::RoundToInt(0.25f / Dt); ++Step)
		{
			Sustained = ExoneerAttitude::AdvanceSustainedTorque(Sustained,
				FVector(0.f, Spike, 0.f), Dt, RoverOffloadTimeConstant);
		}
		const float PassedQuarter = static_cast<float>(Sustained.Y) / Spike;
		for (int32 Step = 0; Step < FMath::RoundToInt(5.75f / Dt); ++Step)
		{
			Sustained = ExoneerAttitude::AdvanceSustainedTorque(Sustained,
				FVector(0.f, Spike, 0.f), Dt, RoverOffloadTimeConstant);
		}
		const float PassedSix = static_cast<float>(Sustained.Y) / Spike;
		AddInfo(FString::Printf(TEXT("the offload lag passes %.0f%% of a 0.25 s input and %.0f%% of a 6 s hold"),
			PassedQuarter * 100.f, PassedSix * 100.f));
		TestTrue(FString::Printf(TEXT("a 0.25 s stick input passes only %.0f%% to the valves"), PassedQuarter * 100.f),
			PassedQuarter < 0.2f);
		TestTrue(FString::Printf(TEXT("a 6 s hold passes %.0f%%"), PassedSix * 100.f), PassedSix > 0.95f);
	}

	// AND THE SUSTAINED HALF: it moves valves, and it still does not move the
	// craft. This is the offload that keeps the yaw axis alive.
	{
		TArray<FTrimEffector> Offloading = MakeRoverThrusters(RoverHoverCollective, 0.f);
		const float Before = TotalLiftN(Offloading);
		FVector Axes[2];
		RoverNullAxes(Axes);
		// The real yaw hold cost at the commanded ceiling.
		const FVector Sustained(0.f, 0.f, RoverHullAngularDamping * RoverInertiaKgM2.Z
			* FMath::DegreesToRadians(RoverRateCeilingDegPerSec));
		bool bMovedAValve = false;
		for (int32 Step = 0; Step < 300; ++Step)
		{
			RouteTrimFrame(Offloading, FVector::ZeroVector, FVector::ZeroVector, Dt, Sustained);
		}
		for (const FTrimEffector& Effector : Offloading)
		{
			bMovedAValve = bMovedAValve || FMath::Abs(Effector.Trim) > 1e-3f;
		}
		const FVector Delivered = DeliveredTrimTorqueNm(Offloading);
		TestTrue(TEXT("a sustained command does move the valves"), bMovedAValve);
		// The allocator is solving three moment axes at once against two force
		// constraints and eight bounded valves, so it lands close rather than
		// exact - the remainder is what the rotors still pay for, and at this
		// size it is a fraction of a percent of the envelope per second.
		AddInfo(FString::Printf(
			TEXT("thrust carries %.0f of the %.0f N*m yaw hold; the %.0f N*m remainder costs the rotors ")
			TEXT("%.2f%% of the yaw envelope per second"),
			Delivered.Z, Sustained.Z, Sustained.Z - Delivered.Z,
			100.f * FMath::Abs(static_cast<float>(Sustained.Z - Delivered.Z)) / RoverCapacityNms));
		TestTrue(FString::Printf(TEXT("and thrust carries %.0f%% of the %.0f N*m yaw hold"),
				100.f * static_cast<float>(Delivered.Z / Sustained.Z), Sustained.Z),
			Delivered.Z > Sustained.Z * 0.8f);
		TestEqual(TEXT("with total lift unchanged"), TotalLiftN(Offloading), Before, 3.f);
		TestTrue(FString::Printf(TEXT("and no forward force (%.2f N)"),
				NetTrimForceAlongN(Offloading, Axes[1])),
			FMath::Abs(NetTrimForceAlongN(Offloading, Axes[1])) < 5.f);
		TestTrue(FString::Printf(TEXT("and only %.1f N sideways, which is the axis the null does not hold"),
				TrimLateralLeakN(Offloading)),
			TrimLateralLeakN(Offloading) < 60.f);
	}

	// The rotor store below the onset must not start a dump either - the dump
	// is a near-saturation recovery, not part of ordinary flying.
	for (const float Saturation : { 0.f, 0.1f, 0.5f, RoverDumpOnsetFraction })
	{
		TestFalse(FString::Printf(TEXT("no dump at %.2f saturation"), Saturation),
			ShouldDesaturate(Saturation, RoverDumpOnsetFraction, RoverDumpReleaseFraction,
				RoverDumpRatePerSec, false));
	}
	TestFalse(TEXT("no dump when the craft has none authored"),
		ShouldDesaturate(1.f, RoverDumpOnsetFraction, RoverDumpReleaseFraction, 0.f, false));
	TestTrue(TEXT("a dump only past the onset"),
		ShouldDesaturate(RoverDumpOnsetFraction + 0.01f, RoverDumpOnsetFraction,
			RoverDumpReleaseFraction, RoverDumpRatePerSec, false));

	// HYSTERESIS. Once started, the dump runs DOWN to the release fraction
	// instead of stopping the instant it dips under the onset - which is what
	// left the pitch axis parked at 80 percent of its envelope for the rest of
	// a flight, i.e. 45 percent of the commanded rate in one direction.
	{
		TestTrue(TEXT("a running dump keeps running below the onset"),
			ShouldDesaturate(RoverDumpOnsetFraction - 0.05f, RoverDumpOnsetFraction,
				RoverDumpReleaseFraction, RoverDumpRatePerSec, true));
		TestTrue(TEXT("all the way down to the release fraction"),
			ShouldDesaturate(RoverDumpReleaseFraction + 0.01f, RoverDumpOnsetFraction,
				RoverDumpReleaseFraction, RoverDumpRatePerSec, true));
		TestFalse(TEXT("and then stops"),
			ShouldDesaturate(RoverDumpReleaseFraction - 0.01f, RoverDumpOnsetFraction,
				RoverDumpReleaseFraction, RoverDumpRatePerSec, true));
		TestTrue(TEXT("the release fraction sits below the onset, or the dump never latches off"),
			RoverDumpReleaseFraction < RoverDumpOnsetFraction);
	}
	return true;
}

/**
 * The one path allowed to move a valve for an attitude REASON - unwinding
 * stored rotor momentum - must be zero-sum in total lift, because it runs
 * while the pilot is flying and he cannot fly around an altitude that moves on
 * its own. And it has to actually reach: B3 was that the dump measured its own
 * headroom from the torque the rate law ASKED for rather than the torque a
 * saturated triad can deliver, so it read zero exactly when it was needed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerThrustDesaturationIsLiftNeutral, "Exoneer.Thrust.DesaturationIsLiftNeutral",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerThrustDesaturationIsLiftNeutral::RunTest(const FString& Parameters)
{
	constexpr float Dt = 1.f / 60.f;

	// A FULLY SATURATED PITCH AXIS with the rate law still screaming for full
	// torque - the exact state in which the old headroom read zero.
	{
		TArray<FTrimEffector> Effectors = MakeRoverThrusters(RoverHoverCollective, 0.5f);
		const float LiftBefore = TotalLiftN(Effectors);
		const FVector Stored(0.f, RoverCapacityNms, 0.f);
		// The rotor winds OPPOSITE the torque it delivers, so a positive store
		// is blocked against a NEGATIVE command. This is the state B3 was
		// about: the rate law keeps asking for full rating on an axis that can
		// deliver none of it.
		const FVector SaturatedAsk(0.f, -RoverRatedTorqueNm, 0.f);

		// B3 IN ONE LINE. Headroom taken from the ASK is zero here, because the
		// ask is the full rating; headroom taken from what the triad will
		// really DELIVER is the whole rating, because a blocked axis delivers
		// nothing. The dump used to throttle itself to nothing at exactly this
		// moment.
		const float HeadroomFromTheAsk = FMath::Max(0.f,
			RoverRatedTorqueNm - FMath::Abs(static_cast<float>(SaturatedAsk.Y)));
		const FVector Deliverable = ExoneerAttitude::ApplySaturation(SaturatedAsk, Stored, RoverCapacityNms);
		const float HeadroomFromDelivery = FMath::Max(0.f,
			RoverRatedTorqueNm - FMath::Abs(static_cast<float>(Deliverable.Y)));
		TestEqual(TEXT("headroom read off the ask is zero on a saturated axis"), HeadroomFromTheAsk, 0.f);
		TestEqual(TEXT("headroom read off deliverable torque is the whole rating"),
			HeadroomFromDelivery, RoverRatedTorqueNm, 1e-3f);

		FVector Delivered = FVector::ZeroVector;
		for (int32 Step = 0; Step < 120; ++Step)
		{
			RouteTrimFrame(Effectors, Stored, SaturatedAsk, Dt);
			TestEqual(TEXT("desaturating does not change total lift"), TotalLiftN(Effectors), LiftBefore, 3.f);
			for (const FTrimEffector& Effector : Effectors)
			{
				TestTrue(TEXT("no valve is biased past its authored authority"),
					Effector.Trim <= TrimBoundMax(Effector) + 1e-5f
					&& Effector.Trim >= TrimBoundMin(Effector) - 1e-5f);
				TestTrue(TEXT("no valve is driven outside 0..1"),
					Effector.BaseThrottle + Effector.Trim >= -1e-5f
					&& Effector.BaseThrottle + Effector.Trim <= 1.f + 1e-5f);
			}
			Delivered = DeliveredTrimTorqueNm(Effectors);
		}
		// The delivered moment must oppose the stored momentum on the saturated
		// axis, which is what unwinding means. With the headroom taken from the
		// ASK this was exactly zero.
		// And with the real headroom the dump actually reaches: the pitch trim
		// opposes the stored momentum by hundreds of N*m, not by nothing.
		TestTrue(FString::Printf(TEXT("the pitch trim opposes the stored momentum (%.1f N*m against %.0f N*m*s)"),
				Delivered.Y, Stored.Y),
			Delivered.Y < -300.f);
	}

	// The same craft with the rotors INSIDE the envelope must show no unwind at
	// all - only the standing-moment trim, which is a different job.
	{
		TArray<FTrimEffector> Quiet = MakeRoverThrusters(RoverHoverCollective, 0.5f);
		TArray<FTrimEffector> Dumping = MakeRoverThrusters(RoverHoverCollective, 0.5f);
		for (int32 Step = 0; Step < 120; ++Step)
		{
			RouteTrimFrame(Quiet, FVector(0.f, RoverCapacityNms * 0.3f, 0.f), FVector::ZeroVector, Dt);
			RouteTrimFrame(Dumping, FVector(0.f, RoverCapacityNms, 0.f), FVector::ZeroVector, Dt);
		}
		bool bDiffers = false;
		for (int32 Index = 0; Index < Quiet.Num(); ++Index)
		{
			bDiffers = bDiffers || FMath::Abs(Quiet[Index].Trim - Dumping[Index].Trim) > 1e-3f;
		}
		TestTrue(TEXT("a saturated craft trims differently from a fresh one"), bDiffers);
	}

	// B10. THE VISOR'S RESIDUAL IS MEASURED AGAINST THE REQUEST THAT WAS MADE,
	// not against the standing moment. While desaturating the request is
	// -Standing - Unwind, so reading |Standing + Trim| returned -Unwind - up to
	// 560 N*m - for a trim that had done exactly what it was told. The visor
	// then printed "UNBALANCED, 0 s of gyro" in pulsing red at precisely the
	// moment the recovery mechanism was working, which is how a warning gets
	// ignored before its one true instance.
	{
		TArray<FTrimEffector> Effectors = MakeRoverThrusters(RoverHoverCollective, 0.f);
		// PITCH, because that is where the shipped rover has the authority to
		// deliver a whole unwind. On yaw at the 0.8 onset the ask is
		// 0.8 x 1600 x 0.35 = 448 N*m against about 400 N*m of canted-nozzle
		// couple, so part of a yaw dump really is un-met and the corrected
		// readout says so - which is the point of measuring it this way.
		const FVector Stored(0.f, RoverCapacityNms * 0.9f, 0.f);
		bool bLatch = false;
		FVector Request = FVector::ZeroVector;
		FVector OldReadout = FVector::ZeroVector;
		FVector Delivered = FVector::ZeroVector;
		for (int32 Step = 0; Step < 300; ++Step)
		{
			OldReadout = RouteTrimFrame(Effectors, Stored, FVector::ZeroVector, Dt,
				FVector::ZeroVector, &bLatch, nullptr, &Request);
			Delivered = DeliveredTrimTorqueNm(Effectors);
		}
		TestTrue(TEXT("the craft is genuinely desaturating"), bLatch);
		const float NewReadout = static_cast<float>((Request - Delivered).GetAbsMax());
		AddInfo(FString::Printf(
			TEXT("a successful pitch unwind: request %.0f N*m, delivered %.0f N*m; ")
			TEXT("the old readout said %.0f N*m UNBALANCED, the corrected one says %.1f N*m"),
			Request.Y, Delivered.Y, OldReadout.GetAbsMax(), NewReadout));
		TestTrue(FString::Printf(TEXT("the unwind is actually being delivered (%.0f of %.0f N*m)"),
				Delivered.Y, Request.Y),
			FMath::Abs(Request.Y) > 100.f && FMath::Abs(Delivered.Y - Request.Y) < 5.f);
		TestTrue(FString::Printf(TEXT("the OLD readout cried wolf at %.0f N*m"), OldReadout.GetAbsMax()),
			OldReadout.GetAbsMax() > 100.f);
		TestTrue(FString::Printf(TEXT("the corrected readout is %.1f N*m, i.e. nothing is wrong"), NewReadout),
			NewReadout < 5.f);
	}
	return true;
}

/**
 * The valve is an actuator, not a switch, on the trim path too: the bias is
 * rate-limited at the same authored slew rate as the lift setting, so a trim
 * cannot step a thruster's throttle in one frame. The limit is a uniform scale
 * of the whole step rather than a per-unit clamp, which is what keeps a
 * lift-neutral solution lift-neutral after limiting.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerThrustTrimSlewIsBounded, "Exoneer.Thrust.TrimSlewIsBounded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerThrustTrimSlewIsBounded::RunTest(const FString& Parameters)
{
	for (const float Fps : { 120.f, 60.f, 20.f })
	{
		const float Dt = 1.f / Fps;
		const float MaxStep = ValveSlewPerSec * Dt;
		TArray<FTrimEffector> Effectors = MakeRoverThrusters(RoverHoverCollective, 0.5f);
		const float LiftBefore = TotalLiftN(Effectors);

		for (int32 Step = 0; Step < 120; ++Step)
		{
			TArray<float> Before;
			for (const FTrimEffector& Effector : Effectors)
			{
				Before.Add(Effector.Trim);
			}
			// Alternate between dumping and not, which is the worst case for a
			// step: the target snaps between a full bias and the standing trim.
			RouteTrimFrame(Effectors,
				(Step % 20) < 10 ? FVector(0.f, RoverCapacityNms, 0.f) : FVector::ZeroVector,
				FVector::ZeroVector, Dt);
			for (int32 Index = 0; Index < Effectors.Num(); ++Index)
			{
				const float Moved = FMath::Abs(Effectors[Index].Trim - Before[Index]);
				// The lift null may add a little on top of the rate limit; it
				// is bounded by the same box and is not a step in its own right.
				TestTrue(FString::Printf(TEXT("unit %d moved %.4f at %.0f fps, limit about %.4f"),
						Index, Moved, Fps, MaxStep),
					Moved <= MaxStep * 2.f + 1e-4f);
				TestTrue(FString::Printf(TEXT("and unit %d is inside its bound after the slew"), Index),
					Effectors[Index].Trim <= TrimBoundMax(Effectors[Index]) + 1e-6f
					&& Effectors[Index].Trim >= TrimBoundMin(Effectors[Index]) - 1e-6f);
			}
			TestEqual(FString::Printf(TEXT("lift holds through a snapping target at %.0f fps"), Fps),
				TotalLiftN(Effectors), LiftBefore, 3.f);
		}
	}

	// A brownout must not make the allocator over-report itself: the moments
	// are priced at the DELIVERABLE thrust, so half power halves them.
	{
		const TArray<FTrimEffector> Full = MakeRoverThrusters(RoverHoverCollective, 0.5f, 1.f);
		const TArray<FTrimEffector> Half = MakeRoverThrusters(RoverHoverCollective, 0.5f, 0.5f);
		for (int32 Index = 0; Index < Full.Num(); ++Index)
		{
			TestEqual(TEXT("half supply halves the priced moment"),
				Half[Index].MomentPerUnitThrottle, Full[Index].MomentPerUnitThrottle * 0.5f, 1e-2f);
			TestEqual(TEXT("half supply halves the priced lift"),
				Half[Index].LiftPerUnitThrottle, Full[Index].LiftPerUnitThrottle * 0.5f, 1e-2f);
		}
	}

	// The authored constants the tests pin have to be the ones on the block.
	if (const UVehicleBlockDefinitionDataAsset* Defaults = GetDefault<UVehicleBlockDefinitionDataAsset>())
	{
		TestEqual(TEXT("the slew rate matches the block default"),
			Defaults->ThrottleSlewPerSec, ValveSlewPerSec, 1e-4f);
		TestEqual(TEXT("the trim fraction matches the block default"),
			Defaults->AttitudeTrimFraction, TrimFractionAuthored, 1e-4f);
		TestEqual(TEXT("the control reserve matches the block default"),
			Defaults->LiftControlReserveFraction, ReserveFractionAuthored, 1e-4f);
		TestEqual(TEXT("the hover gain matches the block default"),
			Defaults->LiftHoverDampingPerMS, HoverDampingAuthored, 1e-4f);
		TestTrue(TEXT("a control reserve exists at all"), Defaults->LiftControlReserveFraction > 0.f);
		TestEqual(TEXT("the nozzle cant matches the block default"),
			Defaults->NozzleCantDeg, NozzleCantDegAuthored, 1e-4f);
		TestEqual(TEXT("the governed descent rate matches the block default"),
			Defaults->LiftDescentRateMS, DescentRateAuthored, 1e-4f);
		TestEqual(TEXT("the dump release fraction matches the block default"),
			Defaults->MomentumDumpReleaseFraction, RoverDumpReleaseFraction, 1e-4f);
		TestEqual(TEXT("the sustained-turn budget matches the block default"),
			Defaults->AttitudeSustainedTurnSeconds, RoverSustainedTurnSeconds, 1e-4f);
		TestEqual(TEXT("the offload lag matches the block default"),
			Defaults->AttitudeOffloadTimeConstantSeconds, RoverOffloadTimeConstant, 1e-4f);
		TestEqual(TEXT("the command rate ceiling matches the block default"),
			Defaults->AttitudeCommandRateCeilingDegPerSec, RoverRateCeilingDegPerSec, 1e-4f);
		TestEqual(TEXT("and the level-return rate is the same number as the ceiling"),
			Defaults->AttitudeLevelRateDegPerSec, Defaults->AttitudeCommandRateCeilingDegPerSec, 1e-4f);
	}
	return true;
}

/**
 * B4, closed. THE LOOP THE VEHICLE ACTUALLY RUNS, on the craft the owner
 * actually flies, for the two minutes the fix has to survive.
 *
 * The test that used to close the momentum loop was parameterised on a trim
 * authority of 2992 N*m, described as what the six lift thrusters have at
 * hover. Nothing produced that number: the trim bound is symmetric, so the
 * authority is a function of the collective, and at the collectives the shipped
 * control scheme could actually reach it was often zero. A recovery proved
 * against an authority the vehicle cannot have is not a proof.
 *
 * So this runs everything at once and reads the rotor store off the end of it:
 *   - the rate law with gains derived from the rover's measured inertia;
 *   - the pilot's rate ceilings from the real momentum budget;
 *   - per-axis rated clamping, per-axis momentum integration and the real
 *     saturation rule;
 *   - the standing moment of the pilot's own throttles about the derived
 *     centre of mass, changing as the valve and the forward key move;
 *   - the trim authority that is genuinely available at that collective,
 *     because the allocator is run on the real thruster set;
 *   - the valve slew, the lift null, and the headroom taken from deliverable
 *     torque rather than from the ask.
 *
 * The profile is an ordinary sortie: climb, hover, translate out, pitch, roll,
 * THIRTY SECONDS OF CONTINUOUS FULL-RATE YAW, hover, translate back, pitch,
 * climb, hover, governed descent, land.
 *
 * That yaw phase is the whole point of the rewrite. The old profile spent 4 s
 * at 0.8 of the yaw limit - 91 degrees, 62 percent of the envelope - and the
 * guard it passed was written against I*w alone, so it could not see that
 * HOLDING the rate costs a further D*I*w every second and that one continuous
 * 202 degree turn therefore ended the axis. The two things that fix it are
 * both exercised here: the rate ceiling now budgets the hold cost, and the
 * sustained part of the command is offloaded to the canted lift nozzles, which
 * is the only in-air yaw effector the craft has.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerThrustRoverHoldsAuthorityForTwoMinutes, "Exoneer.Thrust.RoverHoldsAuthorityForTwoMinutes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerThrustRoverHoldsAuthorityForTwoMinutes::RunTest(const FString& Parameters)
{
	constexpr float Dt = 1.f / 60.f;

	ExoneerAttitude::FLoopParams Loop;
	Loop.InertiaKgM2 = RoverInertiaKgM2;
	Loop.SettleTimeSeconds = RoverSettleTimeSeconds;
	Loop.DampingRatio = 1.f;
	Loop.HullAngularDamping = RoverHullAngularDamping;
	const FVector Kd = ExoneerAttitude::RateGain(Loop);
	const FVector RateLimit = ExoneerAttitude::MaxCommandRateRadS(Loop, RoverCapacityNms,
		RoverCommandMomentumFraction, FMath::DegreesToRadians(RoverRateCeilingDegPerSec),
		RoverSustainedTurnSeconds);

	struct FSortie
	{
		float UntilSeconds = 0.f;
		bool bLiftHeld = false;
		bool bDescendHeld = false;
		float Forward = 0.f;
		/** BODY axes (roll, pitch, yaw), as fractions of that axis rate limit. */
		FVector RateFraction = FVector::ZeroVector;
	};
	// Fractions of the rate limit rather than raw rates, because a fraction is
	// what the stick sends and the limit is what the momentum budget allows.
	// THE YAW PHASE IS THIRTY SECONDS AT FULL DEFLECTION, and that is the
	// change that makes this test measure B7 at all. The previous profile spent
	// 4 s at 0.8 of the yaw limit, which is 91 degrees and 62 percent of the
	// envelope - so the guard passed while the extrapolation to a single
	// 180 degree turn peaked at 95 to 100 percent and a continuous survey turn
	// saturated after 202 degrees. Thirty seconds at full deflection is over
	// 500 degrees of one-directional yaw, which is what a pilot looking around
	// actually does.
	const TArray<FSortie> Sortie =
	{
		{  8.f,  true, false, 0.f, FVector::ZeroVector },              // climb out
		{ 16.f, false, false, 0.f, FVector::ZeroVector },              // hover
		{ 40.f, false, false, 1.f, FVector::ZeroVector },              // translate, 24 s of held W
		{ 44.f, false, false, 1.f, FVector(0.f, 0.6f, 0.f) },          // pitch while translating
		{ 48.f, false, false, 1.f, FVector(0.5f, 0.f, 0.f) },          // roll
		{ 78.f, false, false, 0.f, FVector(0.f, 0.f, 1.f) },           // 30 s of CONTINUOUS full-rate yaw
		{ 86.f, false, false, 0.f, FVector::ZeroVector },              // hover
		{110.f, false, false, 1.f, FVector::ZeroVector },              // translate back
		{114.f, false, false, 1.f, FVector(0.f, -0.6f, 0.f) },         // pitch back
		{126.f,  true, false, 0.f, FVector::ZeroVector },              // climb
		{138.f, false, false, 0.f, FVector::ZeroVector },              // hover
		{150.f, false,  true, 0.f, FVector::ZeroVector },              // governed descent, then land
	};
	constexpr float SortieSeconds = 150.f;

	float Valve = 0.f;
	float VerticalSpeedMS = 0.f;
	FVector BodyRate = FVector::ZeroVector;
	FVector Stored = FVector::ZeroVector;
	FVector Sustained = FVector::ZeroVector;
	FVector PeakStored = FVector::ZeroVector;
	FVector PeakRate = FVector::ZeroVector;
	float WorstResidualNm = 0.f;
	// The router's own DISPLAY filter, two seconds, and therefore the number
	// the pilot is actually shown. The instantaneous residual spikes for a
	// frame or two every time the trim vector has to move, because
	// SlewTrimTowardsRequest holds the whole step to the valve's authored rate -
	// that is a valve, not an imbalance, and averaging is what tells them apart.
	float FilteredResidualNm = 0.f;
	float WorstFilteredResidualNm = 0.f;
	float WorstLateralLeakN = 0.f;
	float FirstSaturationSeconds = -1.f;
	float YawTravelDeg = 0.f;
	bool bDesaturatingLatch = false;
	TArray<FTrimEffector> Effectors = MakeRoverThrusters(0.f, 0.f);
	TArray<float> CarriedTrim;
	CarriedTrim.Init(0.f, Effectors.Num());
	FVector NullAxes[2];
	RoverNullAxes(NullAxes);

	const int32 Steps = FMath::RoundToInt(SortieSeconds / Dt);
	for (int32 Step = 0; Step < Steps; ++Step)
	{
		const float Time = Step * Dt;
		const FSortie* Phase = nullptr;
		for (const FSortie& Candidate : Sortie)
		{
			if (Time < Candidate.UntilSeconds)
			{
				Phase = &Candidate;
				break;
			}
		}
		if (!Phase)
		{
			Phase = &Sortie.Last();
		}

		// --- the lift valve: up, down at a governed rate, or hold ---
		const float Target = Phase->bLiftHeld
			? RoverCeiling
			: HoverCollective(RoverWeightN, RoverLiftScaleN, RoverLiftScaleN, VerticalSpeedMS,
				Phase->bDescendHeld ? -DescentRateAuthored : 0.f,
				HoverDampingAuthored, RoverCeiling, Valve);
		Valve = AdvanceCollective(Valve, Target, Dt, ValveSlewPerSec);

		// --- rebuild the thruster set at this frame's settings ---
		Effectors = MakeRoverThrusters(Valve, Phase->Forward * RoverCeiling);
		for (int32 Index = 0; Index < Effectors.Num(); ++Index)
		{
			// The slew origin is last frame's COMMITTED bias, which the router
			// carries in UThrusterModule::AttitudeTrim. RouteTrimFrame takes it
			// from Trim, so hand it back there - handing it to PreviousTrim
			// instead lets RouteTrimFrame overwrite it with zero, and the valve
			// then never gets more than one frame of slew away from neutral.
			Effectors[Index].Trim = CarriedTrim[Index];
		}

		// --- the rate law, and the trim pipeline the router runs ---
		const FVector Command(Phase->RateFraction.X * RateLimit.X,
			Phase->RateFraction.Y * RateLimit.Y, Phase->RateFraction.Z * RateLimit.Z);
		const FVector AttitudeTorque = ((Command - BodyRate) * Kd).BoundToCube(RoverRatedTorqueNm);
		// The offload the router runs: the part of the command that does not
		// decay goes to thrust, because a rotor holding it fills on a stopwatch.
		Sustained = ExoneerAttitude::AdvanceSustainedTorque(Sustained, AttitudeTorque, Dt,
			RoverOffloadTimeConstant);
		FVector Request = FVector::ZeroVector;
		RouteTrimFrame(Effectors, Stored, AttitudeTorque, Dt, Sustained, &bDesaturatingLatch,
			NullAxes, &Request);
		const FVector TrimTorque = DeliveredTrimTorqueNm(Effectors);
		const FVector Standing = StandingMomentNm(Effectors);
		for (int32 Index = 0; Index < Effectors.Num(); ++Index)
		{
			CarriedTrim[Index] = Effectors[Index].Trim;
		}
		// The visor's number, measured the corrected way: what was ASKED of
		// the trim and not delivered.
		const float InstantResidualNm = static_cast<float>((Request - TrimTorque).GetAbsMax());
		WorstResidualNm = FMath::Max(WorstResidualNm, InstantResidualNm);
		FilteredResidualNm += (InstantResidualNm - FilteredResidualNm) * FMath::Clamp(Dt / 2.f, 0.f, 1.f);
		WorstFilteredResidualNm = FMath::Max(WorstFilteredResidualNm, FilteredResidualNm);
		WorstLateralLeakN = FMath::Max(WorstLateralLeakN, TrimLateralLeakN(Effectors));

		// --- the triad, with both thrust moments fed forward ---
		FVector Gyro = (AttitudeTorque - Standing - TrimTorque).BoundToCube(RoverRatedTorqueNm);
		Gyro = ExoneerAttitude::ApplySaturation(Gyro, Stored, RoverCapacityNms);

		// --- the hull, and the rotors ---
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			const double Total = Standing[Axis] + TrimTorque[Axis] + Gyro[Axis];
			BodyRate[Axis] += (Total / RoverInertiaKgM2[Axis] - Loop.HullAngularDamping * BodyRate[Axis]) * Dt;
			Stored[Axis] = FMath::Clamp(static_cast<float>(Stored[Axis] - Gyro[Axis] * Dt),
				-RoverCapacityNms, RoverCapacityNms);
			PeakStored[Axis] = FMath::Max(PeakStored[Axis], FMath::Abs(Stored[Axis]));
			PeakRate[Axis] = FMath::Max(PeakRate[Axis], FMath::Abs(BodyRate[Axis]));
		}
		if (FirstSaturationSeconds < 0.f && PeakStored.GetAbsMax() >= RoverCapacityNms * 0.999f)
		{
			FirstSaturationSeconds = Time;
		}
		YawTravelDeg += FMath::RadiansToDegrees(static_cast<float>(BodyRate.Z)) * Dt;

		// --- vertical, so the hover phases are the real ones ---
		VerticalSpeedMS += (TotalLiftN(Effectors) / RoverMassKg - PlanetGravityMS2) * Dt;
	}

	// THE HARD REQUIREMENT: no axis loses authority in two minutes of ordinary
	// flying. The store is reported as a fraction of the envelope, because the
	// only number that matters is how close it came to the stop.
	//
	// Logged unconditionally, not just on failure: the point of this test is
	// the numbers, and a green tick with no numbers is what let eight passing
	// tests coexist with an unflyable craft.
	const FVector Fraction = PeakStored / RoverCapacityNms;
	AddInfo(FString::Printf(
		TEXT("%.0f s sortie with %.0f deg of net yaw: peak rotor store roll %.0f / pitch %.0f / yaw %.0f N*m*s ")
		TEXT("(%.0f%% / %.0f%% / %.0f%% of a %.0f N*m*s envelope); ")
		TEXT("rate limits %.1f / %.1f / %.1f deg/s, peak body rate %.1f / %.1f / %.1f deg/s; ")
		TEXT("worst un-met trim request %.0f N*m instantaneous / %.1f N*m sustained; worst lateral trim leak %.0f N"),
		SortieSeconds, YawTravelDeg, PeakStored.X, PeakStored.Y, PeakStored.Z,
		Fraction.X * 100.f, Fraction.Y * 100.f, Fraction.Z * 100.f, RoverCapacityNms,
		FMath::RadiansToDegrees(RateLimit.X), FMath::RadiansToDegrees(RateLimit.Y),
		FMath::RadiansToDegrees(RateLimit.Z),
		FMath::RadiansToDegrees(PeakRate.X), FMath::RadiansToDegrees(PeakRate.Y),
		FMath::RadiansToDegrees(PeakRate.Z), WorstResidualNm, WorstFilteredResidualNm,
		WorstLateralLeakN));
	TestTrue(FString::Printf(TEXT("no axis saturates in %.0f s (peak store roll %.0f%%, pitch %.0f%%, yaw %.0f%% of envelope)"),
			SortieSeconds, Fraction.X * 100.f, Fraction.Y * 100.f, Fraction.Z * 100.f),
		FirstSaturationSeconds < 0.f);
	TestTrue(FString::Printf(TEXT("and every axis keeps real margin: worst is %.0f%%"), Fraction.GetAbsMax() * 100.f),
		Fraction.GetAbsMax() < 0.6f);

	// THE PHASE THAT USED TO KILL YAW. Half a thousand degrees of continuous
	// one-directional yaw, which is what a pilot surveying a landing site does,
	// and the axis has to come out of it with an envelope rather than a stop.
	TestTrue(FString::Printf(TEXT("the sortie really yaws through %.0f degrees in one direction"), YawTravelDeg),
		FMath::Abs(YawTravelDeg) > 400.f);
	TestTrue(FString::Printf(TEXT("and yaw still holds %.0f%% of its envelope in reserve"),
			(1.f - Fraction.Z) * 100.f),
		Fraction.Z < 0.5f);

	// THE HOLD COST IS BUDGETED, not ignored. Spin-up plus the authored turn
	// time out of the rotors alone must fit inside the command budget, which is
	// the arithmetic the previous rate limit skipped entirely.
	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		const float SpinUpNms = static_cast<float>(RoverInertiaKgM2[Axis] * RateLimit[Axis]);
		const float HoldNmPerSec = RoverHullAngularDamping * SpinUpNms;
		const float Budget = RoverCapacityNms * RoverCommandMomentumFraction;
		const TCHAR* Names[3] = { TEXT("roll"), TEXT("pitch"), TEXT("yaw") };
		TestTrue(FString::Printf(
				TEXT("%s: %.0f N*m*s to reach %.1f deg/s plus %.0f N*m*s per second to hold it fits %.0f s in a %.0f N*m*s budget"),
				Names[Axis], SpinUpNms, FMath::RadiansToDegrees(RateLimit[Axis]), HoldNmPerSec,
				RoverSustainedTurnSeconds, Budget),
			SpinUpNms + HoldNmPerSec * RoverSustainedTurnSeconds <= Budget * 1.001f);
	}

	// AND THE GROUND TRACK. The lateral axis is the one the null does not hold,
	// so its leak is the honest cost of the arrangement and has to be small.
	TestTrue(FString::Printf(TEXT("the trim never pushes the craft sideways by more than %.0f N (%.3f m/s2)"),
			WorstLateralLeakN, WorstLateralLeakN / RoverMassKg),
		WorstLateralLeakN < 80.f);

	// The store must be spent on TRANSIENTS, not on a standing moment. A
	// standing moment shows up as a store that only ever grows; a transient
	// gives it back when the stick is released.
	//
	// SUSTAINED, not instantaneous, and the distinction is the whole content of
	// the assertion. The instantaneous residual spikes to about 120 N*m for a
	// frame or two at a phase boundary, because the trim vector has to move and
	// SlewTrimTowardsRequest holds the whole step to the valve's authored rate.
	// Averaged over the router's own two-second display filter, the same sortie
	// peaks at 2.8 N*m - against 5044 N*m on the craft with the one-axis forward
	// pair, which is what a real standing moment looks like through the same
	// filter.
	TestTrue(FString::Printf(TEXT("the moment left to the rotors peaks at %.0f N*m for a frame or two"),
			WorstResidualNm),
		WorstResidualNm < 150.f);
	TestTrue(FString::Printf(TEXT("and sustains only %.1f N*m, so it is a valve slewing and not an imbalance"),
			WorstFilteredResidualNm),
		WorstFilteredResidualNm < 20.f);

	// The craft flew rather than tumbled.
	TestTrue(FString::Printf(TEXT("body rates stay inside the commanded band (roll %.1f, pitch %.1f, yaw %.1f deg/s)"),
			FMath::RadiansToDegrees(PeakRate.X), FMath::RadiansToDegrees(PeakRate.Y),
			FMath::RadiansToDegrees(PeakRate.Z)),
		FMath::RadiansToDegrees(PeakRate.GetAbsMax()) < RoverRateCeilingDegPerSec * 1.2f);

	// THE PREVIOUS BUILD, same loop, same profile: it must fail, or this test
	// is not measuring anything.
	{
		FVector PreviousStored = FVector::ZeroVector;
		FVector PreviousRate = FVector::ZeroVector;
		float DiedAt = -1.f;
		TArray<FTrimEffector> Previous = MakePreviousRoverThrusters(RoverHoverCollective, 1.f);
		const int32 ShortRun = FMath::RoundToInt(20.f / Dt);
		for (int32 Step = 0; Step < ShortRun; ++Step)
		{
			// The previous pass held the standing moment with rotor torque:
			// the rate law saw it as a rate error and paid for it in momentum.
			const FVector Standing = StandingMomentNm(Previous);
			FVector Gyro = ((FVector::ZeroVector - PreviousRate) * Kd).BoundToCube(RoverRatedTorqueNm);
			Gyro = ExoneerAttitude::ApplySaturation(Gyro, PreviousStored, RoverCapacityNms);
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				const double Total = Standing[Axis] + Gyro[Axis];
				PreviousRate[Axis] += (Total / RoverInertiaKgM2[Axis] - Loop.HullAngularDamping * PreviousRate[Axis]) * Dt;
				PreviousStored[Axis] = FMath::Clamp(static_cast<float>(PreviousStored[Axis] - Gyro[Axis] * Dt),
					-RoverCapacityNms, RoverCapacityNms);
			}
			if (DiedAt < 0.f && FMath::Abs(PreviousStored.Y) >= RoverCapacityNms * 0.999f)
			{
				DiedAt = Step * Dt;
			}
		}
		TestTrue(FString::Printf(TEXT("the previous build lost pitch after %.1f s of held forward thrust"), DiedAt),
			DiedAt > 0.f && DiedAt < 3.f);
		TestTrue(FString::Printf(TEXT("and then pitched away uncontrolled at %.2f rad/s"), PreviousRate.Y),
			FMath::Abs(PreviousRate.Y) > 1.f);
	}
	return true;
}

/**
 * B14. THE DESCEND KEY MAY NOT KILL THE VALVE WHILE THE CRAFT IS BANKED OUT OF
 * AUTHORITY, because that is the one state with no trim authority and no
 * momentum sink, and it is reached with two keys a pilot holds on every banked
 * approach.
 *
 * The branch was: banked past holding weight AND asked to descend -> valve to
 * its bottom stop. With the valve shut every lift unit has zero DOWN-travel, so
 * TrimBoundMin is 0 on all six, the force null cannot balance an all-positive
 * bias, EnforceForceNeutralTrim zeroes every trim, and a saturated axis then has
 * no sink at all until touchdown. The governor already answers the question the
 * pilot asked - a craft banked out of authority is sinking anyway, so the frozen
 * valve IS the descent - so the special case is gone.
 *
 * The bank held here is 40 degrees, which the authored bank ceiling now stops
 * the STICK from commanding (see Exoneer.Attitude.BankCeilingBoundsTheReference).
 * It is reachable by collision, slope or hard landing, which is exactly why the
 * governor still has to behave in it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerThrustDescendWhileBankedKeepsTheValveOffItsStop,
	"Exoneer.Thrust.DescendWhileBankedKeepsTheValveOffItsStop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerThrustDescendWhileBankedKeepsTheValveOffItsStop::RunTest(const FString& Parameters)
{
	constexpr float Dt = 1.f / 120.f;
	constexpr float HoldSeconds = 4.f;
	constexpr float BankDeg = 40.f;

	// The state is real: 40 degrees IS past the angle the reserved ceiling can
	// hold weight at, so the governor is pinned there.
	TestTrue(FString::Printf(TEXT("%.0f degrees of bank pins the governor"), BankDeg),
		IsHoverGovernorPinned(RoverWeightN, RoverVerticalLiftScaleN(BankDeg), RoverLiftScaleN, RoverCeiling));

	// bKillValve is the branch that was removed; running both is what turns the
	// fix into a measurement.
	float Result[2][4] = {};
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		const bool bKillValve = (Pass == 0);
		float Valve = RoverHoverCollective;
		float LowestValve = Valve;
		float VerticalMS = 0.f;
		float AltitudeM = 0.f;
		float PeakSink = 0.f;
		for (int32 Step = 0; Step < FMath::RoundToInt(40.f / Dt); ++Step)
		{
			const float Seconds = Step * Dt;
			const bool bDescendHeld = Seconds < HoldSeconds;
			// Banked while the key is held, then rolled level over one second.
			const float Deg = bDescendHeld ? BankDeg
				: BankDeg * FMath::Max(0.f, 1.f - (Seconds - HoldSeconds));
			const float Scale = RoverVerticalLiftScaleN(Deg);
			const bool bPinned = IsHoverGovernorPinned(RoverWeightN, Scale, RoverLiftScaleN, RoverCeiling);
			const float Target = (bKillValve && bDescendHeld && bPinned)
				? 0.f
				: HoverCollective(RoverWeightN, Scale, RoverLiftScaleN, VerticalMS,
					bDescendHeld ? -DescentRateAuthored : 0.f, HoverDampingAuthored, RoverCeiling, Valve);
			Valve = AdvanceCollective(Valve, Target, Dt, ValveSlewPerSec);
			LowestValve = FMath::Min(LowestValve, Valve);
			const float AccelMS2 = RoverVerticalForceN(Valve, Deg) / RoverMassKg - PlanetGravityMS2;
			VerticalMS += AccelMS2 * Dt;
			AltitudeM += VerticalMS * Dt;
			PeakSink = FMath::Min(PeakSink, VerticalMS);
			if (Seconds > HoldSeconds + 2.f && VerticalMS >= -0.05f)
			{
				Result[Pass][3] = Seconds;
				break;
			}
		}
		Result[Pass][0] = LowestValve;
		Result[Pass][1] = PeakSink;
		Result[Pass][2] = AltitudeM;
	}

	// THE DEFECT, measured: the valve reaches its bottom stop, the craft
	// free-falls, and it takes half a kilometre to get it back.
	TestTrue(FString::Printf(
			TEXT("the valve kill drove the valve to %.3f, peaked at %.1f m/s of sink and cost %.0f m"),
			Result[0][0], Result[0][1], -Result[0][2]),
		Result[0][0] < 0.01f && Result[0][1] < -30.f && -Result[0][2] > 400.f);

	// THE FIX: the valve never leaves the freeze, so the sink is bounded and
	// the recovery is short.
	TestTrue(FString::Printf(TEXT("the governor holds the valve at %.3f instead of shutting it"), Result[1][0]),
		Result[1][0] > 0.5f);
	TestTrue(FString::Printf(TEXT("which cuts the peak sink from %.1f to %.1f m/s"), Result[0][1], Result[1][1]),
		Result[1][1] > -12.f);
	TestTrue(FString::Printf(TEXT("and the altitude cost from %.0f m to %.0f m, recovered in %.1f s"),
			-Result[0][2], -Result[1][2], Result[1][3]),
		-Result[1][2] < 100.f && Result[1][3] > 0.f && Result[1][3] < 20.f);

	// AND THE REASON IT MATTERS: a valve at its bottom stop has no DOWN-travel,
	// so the force-neutral path collapses and the trim - the craft's only
	// in-air momentum sink - is abandoned entirely.
	{
		FVector Axes[2];
		RoverNullAxes(Axes);
		// A shut LIFT valve with the forward key held: a real standing moment
		// and nothing left to answer it with, because every lift unit's
		// down-travel is zero and a force-neutral bias needs units moving both
		// ways. This is the state the removed branch reached in mid-air.
		TArray<FTrimEffector> Shut = MakeRoverThrusters(0.f, RoverCeiling);
		float ShutTravel = 0.f;
		for (const FTrimEffector& Effector : Shut)
		{
			if (Effector.LiftPerUnitThrottle > 1.f)
			{
				ShutTravel = FMath::Max(ShutTravel, -TrimBoundMin(Effector));
			}
		}
		TestEqual(TEXT("a shut lift valve has no down-travel at all"), ShutTravel, 0.f);
		const FVector ShutStanding = StandingMomentNm(Shut);
		AllocateForceNeutralTrim(Shut, -ShutStanding, Axes, 2);
		const float ShutResidual = (ShutStanding + DeliveredTrimTorqueNm(Shut)).GetAbsMax();
		TestTrue(FString::Printf(
				TEXT("so %.0f N*m of standing moment is left entirely to the rotors (%.0f N*m residual)"),
				ShutStanding.GetAbsMax(), ShutResidual),
			ShutStanding.GetAbsMax() > 100.f && ShutResidual > ShutStanding.GetAbsMax() * 0.9f);

		// The frozen valve keeps travel in both directions, so the same craft
		// cancels the same moment and keeps its momentum sink.
		TArray<FTrimEffector> Frozen = MakeRoverThrusters(Result[1][0], RoverCeiling);
		const FVector FrozenStanding = StandingMomentNm(Frozen);
		AllocateForceNeutralTrim(Frozen, -FrozenStanding, Axes, 2);
		TestTrue(FString::Printf(TEXT("at the frozen %.3f the same moment cancels to %.2f N*m"),
				Result[1][0], (FrozenStanding + DeliveredTrimTorqueNm(Frozen)).GetAbsMax()),
			(FrozenStanding + DeliveredTrimTorqueNm(Frozen)).GetAbsMax() < 1.f);
		TArray<FTrimEffector> Sink = MakeRoverThrusters(Result[1][0], RoverCeiling);
		AllocateForceNeutralTrim(Sink, FVector(0.f, 400.f, 0.f), Axes, 2);
		TestTrue(FString::Printf(TEXT("and an unwind of 400 N*m is still available: %.0f N*m delivered"),
				DeliveredTrimTorqueNm(Sink).Size()),
			DeliveredTrimTorqueNm(Sink).Size() > 350.f);
	}
	return true;
}

/**
 * B16. THE GROUND ROLL MUST NOT SPEND ROTOR MOMENTUM. The lift key opens the
 * valve to the reserved ceiling whether the wheels are down or not, so a craft
 * on the pad with the key held has a REAL standing moment - 123 N*m of roll and
 * 578 N*m of pitch at 0.90 - and gating the cancellation on being airborne fed
 * every newton-metre of it into the rotors.
 *
 * The arithmetic is not marginal. The authored ground bleed is 0.25/s, so its
 * equilibrium against a 578 N*m moment is 578 / 0.25 = 2311 N*m*s, which is
 * ABOVE the 1600 N*m*s the two triads hold: the pitch axis does not settle
 * somewhere unpleasant, it saturates outright. And nothing gives it back, since
 * the residual is zero once airborne (nothing refills) and the dump only arms at
 * 0.8 of the envelope (nothing unwinds).
 *
 * Gating on the VALVE instead is free: the bias is force-nulled, so on the
 * ground it changes no net force and only removes the moment.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerThrustGroundRollWithLiftHeldSpendsNoMomentum,
	"Exoneer.Thrust.GroundRollWithLiftHeldSpendsNoMomentum",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerThrustGroundRollWithLiftHeldSpendsNoMomentum::RunTest(const FString& Parameters)
{
	constexpr float Dt = 1.f / 60.f;
	constexpr float GroundBleedPerSec = 0.25f;   // DA_Block_Gyro momentum_ground_bleed_per_sec

	// The standing moment is real and it is large: this is not a rounding error
	// being cancelled, it is the craft's dominant moment.
	{
		const TArray<FTrimEffector> AtCeiling = MakeRoverThrusters(RoverCeiling, 0.f);
		const FVector Standing = StandingMomentNm(AtCeiling);
		TestTrue(FString::Printf(TEXT("the valve at the ceiling makes %.0f N*m of roll and %.0f N*m of pitch"),
				Standing.X, Standing.Y),
			FMath::Abs(Standing.Y) > 500.f && FMath::Abs(Standing.X) > 100.f);
		const float Equilibrium = FMath::Abs(static_cast<float>(Standing.Y)) / GroundBleedPerSec;
		TestTrue(FString::Printf(
				TEXT("whose ground-bleed equilibrium is %.0f N*m*s against a %.0f N*m*s envelope - it saturates"),
				Equilibrium, RoverCapacityNms),
			Equilibrium > RoverCapacityNms);
	}

	// The ground phase, both gates. A pilot on the pad is holding the lift key
	// and nothing else, so the attitude command is zero and the gyro is left
	// holding exactly whatever thrust did not cancel.
	const auto GroundDwell = [Dt, GroundBleedPerSec](bool bGateOnValve, float DwellSeconds, float& OutValve)
	{
		FVector Stored = FVector::ZeroVector;
		float Valve = 0.f;
		TArray<FTrimEffector> Effectors = MakeRoverThrusters(0.f, 0.f);
		for (int32 Step = 0; Step < FMath::RoundToInt(DwellSeconds / Dt); ++Step)
		{
			Valve = AdvanceCollective(Valve, RoverCeiling, Dt, ValveSlewPerSec);
			TArray<FTrimEffector> Next = MakeRoverThrusters(Valve, 0.f);
			for (int32 Index = 0; Index < Next.Num(); ++Index)
			{
				Next[Index].Trim = Effectors[Index].Trim;
			}
			// bAirTrimActive is false on the ground either way: the rate
			// offload and the dump have the ground itself as their sink.
			const bool bStandingTrimActive = bGateOnValve && Valve > KINDA_SMALL_NUMBER;
			const FVector Residual = RouteTrimFrame(Next, Stored, FVector::ZeroVector, Dt,
				FVector::ZeroVector, nullptr, nullptr, nullptr, bStandingTrimActive, false);
			Effectors = Next;
			// The gyro is handed what thrust did not cancel, and a reaction
			// wheel winds opposite the torque it delivers.
			const FVector Gyro = ExoneerAttitude::ApplySaturation(
				(-Residual).BoundToCube(RoverRatedTorqueNm), Stored, RoverCapacityNms);
			Stored = (Stored - Gyro * Dt) * FMath::Max(0.f, 1.f - GroundBleedPerSec * Dt);
			Stored = Stored.BoundToCube(RoverCapacityNms);
		}
		OutValve = Valve;
		return Stored;
	};

	// A 0.75 s ground roll - what the shipped rover actually takes to break
	// ground - and then the dwells a heavier or lower-TWR craft would sit
	// through. The airborne gate saturates pitch at six seconds.
	for (const float Dwell : { 0.75f, 2.f, 2.5f, 3.5f, 6.f })
	{
		float ValveBefore = 0.f;
		float ValveAfter = 0.f;
		const FVector Airborne = GroundDwell(false, Dwell, ValveBefore);
		const FVector Valved = GroundDwell(true, Dwell, ValveAfter);
		const float BeforePct = 100.f * FMath::Abs(static_cast<float>(Airborne.Y)) / RoverCapacityNms;
		const float AfterPct = 100.f * FMath::Abs(static_cast<float>(Valved.Y)) / RoverCapacityNms;
		TestTrue(FString::Printf(
				TEXT("%.2f s on the pad at valve %.3f: gated on AIRBORNE spends %.0f percent of the pitch envelope"),
				Dwell, ValveBefore, BeforePct),
			BeforePct > 15.f);
		TestTrue(FString::Printf(TEXT("gated on the VALVE it spends %.1f percent"), AfterPct),
			AfterPct < 1.f && FMath::Abs(static_cast<float>(Valved.X)) < RoverCapacityNms * 0.01f);
	}

	// AND IT IS FREE: on the ground the bias changes no net force, so it cannot
	// lift a parked rover or shift its weight onto one axle.
	{
		TArray<FTrimEffector> OnThePad = MakeRoverThrusters(RoverCeiling, 0.f);
		const float LiftBefore = TotalLiftN(OnThePad);
		FVector Axes[2];
		RoverNullAxes(Axes);
		const FVector Standing = StandingMomentNm(OnThePad);
		AllocateForceNeutralTrim(OnThePad, -Standing, Axes, 2);
		TestEqual(FString::Printf(TEXT("the ground trim moves total lift by %.2f N"),
				TotalLiftN(OnThePad) - LiftBefore),
			TotalLiftN(OnThePad), LiftBefore, 3.f);
		TestTrue(FString::Printf(TEXT("and cancels the moment to %.2f N*m"),
				(Standing + DeliveredTrimTorqueNm(OnThePad)).GetAbsMax()),
			(Standing + DeliveredTrimTorqueNm(OnThePad)).GetAbsMax() < 1.f);
	}
	return true;
}

/**
 * B15. A NAIVE BUILD MUST BE VISIBLE TO THE PILOT. The nozzle cant is on the
 * block definition, so every thruster has it; the only thing a placement
 * chooses is which way the jet leans. The build tool's aim list used to offer
 * one entry per aim direction, built from the one-axis orientation lookup, so
 * six units aimed "up" all leaned the SAME way - and the one layout a player
 * could not build was the balanced one the spawner builds.
 *
 * Uniform toe is not a moment, it is a FORCE, which is why nothing already on
 * the visor caught it: the trim path nulls the net TRIM force and never the base
 * throttle's own, flight writes Move.X and Rotate.Z and never Move.Y so there is
 * no lateral thrust command to fight it with, and the momentum store stays
 * clean. The craft simply accelerates sideways for ever.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FExoneerThrustUniformToeIsVisibleToThePilot,
	"Exoneer.Thrust.UniformToeIsVisibleToThePilot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FExoneerThrustUniformToeIsVisibleToThePilot::RunTest(const FString& Parameters)
{
	FVector Axes[2];
	RoverNullAxes(Axes);

	// THE SHIPPED LAYOUT is force-neutral at the hover collective: the two toed
	// rails cancel and so does the mirrored forward pair.
	{
		const TArray<FTrimEffector> Shipped = MakeRoverThrusters(RoverHoverCollective, RoverCeiling);
		TestTrue(FString::Printf(TEXT("the shipped craft makes %.2f N of standing side force"),
				BaseForceAlongN(Shipped, FVector::YAxisVector)),
			FMath::Abs(BaseForceAlongN(Shipped, FVector::YAxisVector)) < 1.f);
	}

	// THE NAIVE BUILD: every lift nozzle toed the same way.
	{
		const TArray<FRoverThruster> Uniform = UniformToeRoverThrusters();
		TArray<FTrimEffector> Naive = MakeRoverThrustersFrom(Uniform, RoverHoverCollective, 0.f);
		const float SideN = BaseForceAlongN(Naive, FVector::YAxisVector);
		const float DriftMS2 = FMath::Abs(SideN) / RoverMassKg;
		TestTrue(FString::Printf(TEXT("six nozzles toed the same way make %.0f N of side thrust, %.2f m/s2 of drift"),
				SideN, DriftMS2),
			FMath::Abs(SideN) > 1500.f && DriftMS2 > 0.8f);

		// It survives the trim, because the trim is force-NEUTRAL by design: it
		// can only remove what it added, never what the pilot's own throttle
		// makes. So no amount of allocation touches it.
		const FVector Standing = StandingMomentNm(Naive);
		AllocateForceNeutralTrim(Naive, -Standing, Axes, 2);
		TestEqual(FString::Printf(TEXT("and the trim cannot touch it (%.0f N before, %.0f N after)"),
				SideN, BaseForceAlongN(Naive, FVector::YAxisVector)),
			BaseForceAlongN(Naive, FVector::YAxisVector), SideN, 1.f);
		TestTrue(FString::Printf(TEXT("while the momentum store stays clean: residual %.2f N*m"),
				(Standing + DeliveredTrimTorqueNm(Naive)).GetAbsMax()),
			(Standing + DeliveredTrimTorqueNm(Naive)).GetAbsMax() < 5.f);
		AddInfo(FString::Printf(
			TEXT("a uniformly toed build flies badly and says so: %.0f N of THRUST BIAS on the visor, ")
			TEXT("nothing on the momentum readout, because it is a force and not a moment"), SideN));
	}

	// THE ONE-AXIS FORWARD PAIR is the same defect on two units instead of six,
	// and there it IS a moment as well as a force - which is what took the yaw
	// axis out on the shipped craft.
	{
		const TArray<FRoverThruster> OneAxis = OneAxisForwardRoverThrusters();
		TArray<FTrimEffector> Defect = MakeRoverThrustersFrom(OneAxis, RoverHoverCollective, RoverCeiling);
		const float SideN = BaseForceAlongN(Defect, FVector::YAxisVector);
		const FVector Standing = StandingMomentNm(Defect);
		AllocateForceNeutralTrim(Defect, -Standing, Axes, 2);
		const float ResidualYaw = FMath::Abs(static_cast<float>(
			(Standing + DeliveredTrimTorqueNm(Defect)).Z));
		TestTrue(FString::Printf(
				TEXT("both forward units leaning the same way: %.0f N of side thrust AND %.0f N*m of untrimmable yaw"),
				SideN, ResidualYaw),
			FMath::Abs(SideN) > 700.f && ResidualYaw > 300.f);
		TestTrue(FString::Printf(TEXT("which is %.1f s of yaw envelope per second of held W"),
				RoverCapacityNms / FMath::Max(ResidualYaw, 1.f)),
			RoverCapacityNms / FMath::Max(ResidualYaw, 1.f) < 6.f);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
