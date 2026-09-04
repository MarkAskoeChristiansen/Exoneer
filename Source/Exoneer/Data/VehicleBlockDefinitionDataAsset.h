// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "ExoneerTypes.h"
#include "Vehicles/VehicleModule.h"   // TSubclassOf<UVehicleModule> needs the complete type
#include "Vehicles/VehicleWheelSpec.h"
#include "VehicleBlockDefinitionDataAsset.generated.h"

class UStaticMesh;
class UTexture2D;

/**
 * Data-driven description of one vehicle block on the unified 25 cm grid.
 * Structural blocks have no ModuleClass; functional blocks (thruster, cockpit,
 * battery, solar) name a UVehicleModule subclass that the construct
 * instantiates server-side. Author instances under
 * /Content/Exoneer/Data/VehicleBlocks/.
 */
UCLASS(BlueprintType)
class EXONEER_API UVehicleBlockDefinitionDataAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** Stable identifier ("frame_1x1", "thruster_small", "cockpit", ...). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, AssetRegistrySearchable, Category = "Block")
	FName BlockId;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Block")
	FText DisplayName;

	/** AABB occupancy in 25 cm cells, before orientation. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Block", meta = (ClampMin = "1"))
	FIntVector SizeInCells = FIntVector(1, 1, 1);

	/** Kilograms; contributes to the construct's rigid body mass and COM. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Stats")
	float Mass = 50.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Stats")
	float MaxHealth = 150.f;

	/** Ghost -> complete investment stages (materials + weld work). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Construction")
	TArray<FConstructionCost> Stages;

	/** Null for structural blocks. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	TSubclassOf<UVehicleModule> ModuleClass;

	/** Watts. Positive produces, negative consumes at full activity. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	float PowerDelta = 0.f;

	/** Watt-seconds a battery block stores (0 for non-batteries). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	float EnergyStorage = 0.f;

	/** Newtons at full throttle (thruster blocks). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	float MaxThrust = 0.f;

	/**
	 * How fast the thruster's throttle valve can slew, in throttle units per
	 * second. The lift setting FOLLOWS the lift key at this rate in both
	 * directions - hold to open, release to close - so thrust cannot step from
	 * zero to full in one frame and letting go always stops it. It also rate-
	 * limits the desaturation bias, the only other thing that moves a valve.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	float ThrottleSlewPerSec = 2.f;

	/**
	 * Nozzle cant: how far the thrust vector sits off the block's aim axis,
	 * rotated toward block local +Y (degrees). The AIM is unchanged - the
	 * nozzle still points out of the block's own -X face, so the build tool's
	 * aim list and the spawner still agree about which orientation means
	 * "thrust up" - but the jet leaves it canted, and rolling the block about
	 * its aim axis chooses which way.
	 *
	 * This is what gives a craft made of parallel lift nozzles any YAW
	 * authority at all. Six thrusters pointing along the hull's own up axis
	 * make exactly zero yaw moment however they are throttled, so yaw had no
	 * in-air actuator and the rotors paid for every degree of it until they
	 * filled. Canted OUTBOARD on each rail, uniform throttle still makes no
	 * yaw (the two rails cancel) while a diagonal trim pattern is a pure yaw
	 * couple with no net force.
	 *
	 * The cost is real and shows on the TWR readout: world-vertical lift falls
	 * by cos^2 of this angle, because the collective is shared out in
	 * proportion to each unit's lift share and then only that share pushes up.
	 * 6 degrees costs 1.1 percent of installed lift and buys, measured on the
	 * test rover at the hover collective, 251 N*m of pure yaw couple at a 0.20
	 * diagonal bias and 302 N*m at the full travel that collective leaves -
	 * against the 81 N*m the axis needs to hold its fastest commanded turn
	 * (0.15 x 1544 x 20 deg/s).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	float NozzleCantDeg = 6.f;

	/**
	 * Descent rate the governor flies while the descend key is held (m/s,
	 * positive number, downward). The key is a RATE COMMAND, not a valve kill.
	 *
	 * Killing the valve made the only descent control unrecoverable: the craft
	 * fell at a full 1 g while the governor's arrest authority was 0.19 g, an
	 * asymmetry of five to one, so four seconds of held Ctrl reached 37 m/s
	 * and needed 26 s and 526 m to stop - and landing damage starts at 8 m/s,
	 * which arrived 0.8 s in. Governed to a bounded rate the same four seconds
	 * reach 2.5 m/s and 7.9 m, and releasing the key levels off in 4.5 s and
	 * 10 m. It also keeps the lift valve off its bottom stop, which is what
	 * keeps the pitch trim alive: Ctrl and W together used to be the one state
	 * with no trim authority at all.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	float LiftDescentRateMS = 2.5f;

	/**
	 * Fraction of rated thrust this unit may spend on trim, i.e. how far its
	 * throttle may be biased away from the pilot's setting to make a moment
	 * about the centre of mass. Reserved for ONE job: unwinding stored rotor
	 * momentum, which a reaction wheel can only do against an external torque.
	 * Attitude transients never come here - the triad is the fast, precise
	 * actuator, and a valve that moves for attitude moves the altitude too.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	float AttitudeTrimFraction = 0.35f;

	/**
	 * Fraction of every valve's travel held back from the PILOT as control
	 * margin, so the differential-thrust path still has authority at the top
	 * of the lever. Trim is symmetric - a unit may only be biased as far as it
	 * can be biased back - so without a reserve a craft at full collective has
	 * exactly zero authority to hold a standing moment or unwind a rotor, and
	 * full collective is where a climbing pilot lives. The cost is honest and
	 * shown: ascent thrust-to-weight is the reserved ceiling times installed
	 * lift. It cannot buy authority at the BOTTOM of the travel - a shut valve
	 * makes no thrust - and the visor warns there instead.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	float LiftControlReserveFraction = 0.1f;

	/**
	 * Vertical-rate gain of the lift governor, in valve fraction per m/s of
	 * climb. Releasing both lift keys airborne leaves the valve seeking the
	 * setting that holds altitude: weight over world-vertical lift, plus this
	 * gain times the vertical speed. The closed-loop time constant is
	 * mass / (lift scale * gain), about 0.8 s on the test rover, so the craft
	 * settles rather than porpoises. There is no position term on purpose: an
	 * altitude integrator is a latch, and a control the pilot cannot switch
	 * off by letting go is not a control.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	float LiftHoverDampingPerMS = 0.1f;

	/**
	 * Rated reaction torque per axis (N*m) for attitude gyro blocks. The
	 * construct sums this over Complete gyros; a vehicle with none has no
	 * attitude authority at all, which is the physical truth.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	float MaxGyroTorqueNm = 0.f;

	/**
	 * Rotor momentum capacity PER AXIS (N*m*s) for attitude gyro blocks: the
	 * I*omega one rotor holds at its speed limit. This is a property of the
	 * hardware inside the box, NOT "rating times some seconds" - a rotor that
	 * fits a 0.5 m, 180 kg block and survives its own rim speed holds a few
	 * hundred N*m*s, whatever its motor is rated for.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	float GyroMomentumCapacityNms = 0.f;

	/**
	 * Closed-loop time constant of the attitude hold this block's controller
	 * runs (s). Gains are derived from it and the hull's measured inertia
	 * (Kp = I/T^2, Kd = (2*zeta/T - hull damping)*I), so the loop is stable
	 * and overshoot-free on any hull the player builds.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	float AttitudeSettleTimeSeconds = 0.25f;

	/** Target closed-loop damping ratio. 1.0 = critically damped = no overshoot. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	float AttitudeDampingRatio = 1.f;

	/**
	 * Comfort ceiling on the pilot's commanded body rate (deg/s), before the
	 * momentum budget is applied. Keep it near what the budget allows on the
	 * heavy axes: a ceiling the light axis reaches while the heavy ones are
	 * momentum-capped at half of it gives the pilot three different craft to
	 * fly at once.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	float AttitudeCommandRateCeilingDegPerSec = 20.f;

	/**
	 * How long a full-deflection turn is paid for out of the ROTORS ALONE (s).
	 *
	 * Holding a body rate is not free: a hull with angular damping D needs
	 * D*I*w of torque to stay at rate w, for ever, and a reaction wheel pays
	 * that in momentum every second. Budgeting only the spin-up cost I*w -
	 * which is what the previous pass did - said the yaw axis could hold
	 * 30 deg/s, while the truth was that one continuous 202 degree turn filled
	 * the rotor and killed the axis. The command rate is now
	 * Fraction*Capacity / (I*(1 + D*T)), so the budget covers the spin-up plus
	 * T seconds of holding it.
	 *
	 * Past T the hold torque is carried by differential thrust instead, on any
	 * axis where the craft's thrust geometry can make that moment. So T is the
	 * guarantee for a craft that has NO such geometry, not a limit on how long
	 * a turn may last.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	float AttitudeSustainedTurnSeconds = 4.f;

	/**
	 * Time constant of the lag that splits the attitude command into a
	 * transient (paid by the rotors, which give it back) and a SUSTAINED part
	 * (offloaded to differential thrust, which can hold it for ever). At 1.5 s
	 * a quarter-second stick input passes about 15 percent to the valves and a
	 * multi-second hold passes all of it, so attitude transients still never
	 * move a valve - and the offload is force-nulled, so nothing it does moves
	 * the altitude or the ground track.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	float AttitudeOffloadTimeConstantSeconds = 1.5f;

	/**
	 * Rate at which the attitude reference returns to LEVEL in roll and pitch
	 * once the stick is released (deg/s). Yaw is never held.
	 *
	 * A thrust vehicle has no restoring moment about its own centre of mass,
	 * so any bank is a permanent sideways acceleration - a 20 degree bump left
	 * the test rover accelerating at 3.6 m/s^2 with the pilot holding nothing,
	 * decaying only into linear damping for a 69 m/s drift. Nothing but the
	 * attitude system can end that, and this is the rate at which it does.
	 * Keep it at the command ceiling so releasing the stick unwinds a bank
	 * exactly as fast as the stick put it in.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	float AttitudeLevelRateDegPerSec = 20.f;

	/**
	 * Largest tilt of the craft's own lift axis away from world up that the
	 * attitude computer will COMMAND (deg). A flight-envelope limiter, not a
	 * force: it bounds the attitude REFERENCE, applies no torque of its own,
	 * and does nothing to stop a collision or a slope putting the hull outside
	 * it - the reference re-seeds from the hull and the pilot flies it back.
	 *
	 * The bound is needed because A/D is FULL-DEFLECTION roll and the reference
	 * had no absolute bound at all, only a 5.0 degree leash to the hull, which
	 * is anti-windup and says nothing about where the hull ends up. Measured on
	 * the shipped rover from 200 m: 3 s of one held key reached 53.9 degrees of
	 * bank, 5 s reached 93.2 degrees, and past 90 the lift valve was pointing at
	 * the ground - 21.3 kN of it, 1.2 g on top of weight and therefore 2.2 g of
	 * downward acceleration, so 10 s cost 288 m and reached -104 m/s against a
	 * landing-damage threshold of 8 m/s. Bounded at 30 degrees the same 10 s of
	 * held D costs 0.1 m of altitude.
	 *
	 * 30 degrees is chosen against the craft, not by taste: the shipped rover's
	 * reserved ceiling holds weight out to acos(18120 / (23738 x 0.90)) = 32.0
	 * degrees, so a 30 degree ceiling keeps every commandable attitude one the
	 * craft can hold its own weight at, and the lift governor can therefore
	 * never be PINNED by the stick alone. It still buys g tan(30) = 5.7 m/s^2
	 * of lateral acceleration, which is how a thruster craft translates.
	 *
	 * A craft with a worse thrust-to-weight ratio holds weight at a smaller
	 * angle than this, so it CAN still be flown into the pinned band and the
	 * visor says PINNED when it is. 0 disables the limiter.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	float AttitudeBankCeilingDeg = 30.f;

	/**
	 * Share of the momentum envelope a full-deflection rate command may spend.
	 * Reaching rate w on an axis costs I*w of rotor momentum, so this is what
	 * actually limits how fast a heavy hull may be asked to turn - and why a
	 * second gyro makes the same hull turn faster.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	float AttitudeCommandMomentumFraction = 0.5f;

	/**
	 * Rate at which stored rotor momentum is unwound against an EXTERNAL
	 * torque (1/s). Never a free bleed: the escape costs thruster authority in
	 * flight, or is reacted through the wheels on the ground.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	float MomentumDumpRatePerSec = 0.35f;

	/**
	 * Saturation fraction above which desaturation starts spending thrust.
	 * MUST sit above AttitudeCommandMomentumFraction, or the dump runs during
	 * every ordinary stick input instead of as a near-saturation recovery -
	 * Exoneer.Attitude.ConstantInvariants asserts exactly that.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	float MomentumDumpOnsetFraction = 0.8f;

	/**
	 * Saturation fraction a dump runs DOWN to once it has started. Without
	 * this hysteresis the unwind stopped the instant saturation fell back
	 * below the onset and the axis parked there: 30 s of continuous pitch left
	 * the store at 80 percent for the rest of the flight, which is 45 percent
	 * of the commanded rate in one direction and not the fresh envelope a
	 * green readout implies. Must sit below MomentumDumpOnsetFraction.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	float MomentumDumpReleaseFraction = 0.4f;

	/**
	 * Fraction of stored rotor momentum shed per second while any wheel is
	 * loaded (1/s). The ground is the one sink that is always available: it
	 * really does supply the external torque, through the tyres. Without it a
	 * saturated triad on a wheel-less craft, or on a tipped rover, can never
	 * recover - saturation stops being a setback and becomes a dead vehicle.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	float MomentumGroundBleedPerSec = 0.25f;

	/**
	 * Debounce on the ground/air decision, BOTH edges (s). The craft counts as
	 * airborne only after this long clear of the ground, and as grounded only
	 * after this long in contact, so a rover bouncing over rough terrain does
	 * not flicker between the two.
	 *
	 * What it gates is the LIFT GOVERNOR, and only that: hover hold must not
	 * open the valve on a craft that is sitting on its wheels, and a single
	 * wheel tap must not slam the valve shut mid-flight. The attitude loop is
	 * deliberately NOT gated on contact - see EPilotControlMode - because the
	 * triad rate-nulls whenever a pilot is aboard, on the ground and in the
	 * air, which is the behaviour the owner had before and did not complain
	 * about. A one-edge debounce was the bug: the release edge was debounced
	 * and the contact edge was not, so one wheel tap cut the loop instantly.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	float AttitudeGroundReleaseSeconds = 0.25f;

	/** Kilograms of propellant a fuel-tank block stores (0 = not a tank). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	float FuelCapacityKg = 0.f;

	/** Primary asset name of the spare item consumed by Replace (wheels: "tire"). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Module")
	FName SpareItemId;

	/** True for wheel blocks: excluded from ISMC visuals, gets a UWheelModule + a dedicated animated mesh component. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheel")
	bool bIsWheel = false;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wheel", meta = (EditCondition = "bIsWheel"))
	FVehicleWheelSpec WheelSpec;

	/**
	 * Skip the world-static overlap rejection when placing this block: wheels
	 * sit at ground level by design and would otherwise be unplaceable on a
	 * hull resting on terrain.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Block")
	bool bAllowTerrainOverlapOnPlace = false;

	/**
	 * Applied between the block's local frame and the mesh, before the
	 * cell-fit scale. Lets the Z-aligned engine cylinder stand in for a
	 * Y-axis wheel (roll 90).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Visual")
	FTransform MeshRelativeTransform = FTransform::Identity;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Visual")
	TSoftObjectPtr<UStaticMesh> Mesh;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Visual")
	TSoftObjectPtr<UTexture2D> Icon;

	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId(TEXT("VehicleBlock"), BlockId);
	}
};
