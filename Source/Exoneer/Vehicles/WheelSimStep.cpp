// Copyright Exoneer contributors.
#include "Vehicles/WheelSimTypes.h"
#include "Vehicles/ExoneerVehicleUnits.h"

namespace ExoneerWheelSim
{

namespace
{
	/**
	 * Dissipative torques (brake, rolling resistance, bearing drag) obey the
	 * no-reversal rule: they can stop the wheel this substep but never spin
	 * it the other way - that is what kills brake limit cycles at rest.
	 */
	float ApplyDissipativeTorque(float Omega, float TorqueNm, float InertiaKgM2, float Dt)
	{
		const float MaxDelta = Dt * TorqueNm / FMath::Max(InertiaKgM2, 1e-3f);
		if (FMath::Abs(Omega) <= MaxDelta)
		{
			return 0.f;
		}
		return Omega - FMath::Sign(Omega) * MaxDelta;
	}
}

FWheelSimForce StepWheel(float Dt, const FWheelSimInputItem& Input, const FWheelBodyView& Body,
	FWheelSimState& State, FWheelSimTelemetry& Telemetry)
{
	using namespace ExoneerTerramechanics;

	FWheelSimForce Out;
	const FWheelSimConfig& Config = Input.Config;
	const FWheelSimCommand& Command = Input.Command;
	const FWheelSimGround& Ground = Input.Ground;
	const float Inertia = FMath::Max(Config.WheelInertiaKgM2, 1e-3f);

	Telemetry.BlockInstanceId = Input.BlockInstanceId;

	// World-space suspension geometry from the body-local cache: it tracks
	// the body through every substep even though the raycast is per frame.
	const FVector MountW = Body.BodyTM.TransformPosition(Ground.MountLocalUU);
	const FVector AxisW = Body.BodyTM.TransformVectorNoScale(Ground.AxisLocal);
	const FVector& Normal = Ground.PlaneNormal;
	const float AxisDotNormal = FVector::DotProduct(AxisW, Normal);

	// Airborne when there is no cached contact, the wheel points away from
	// the plane, or the strut cannot reach it.
	bool bAirborne = !Ground.bHasContact || AxisDotNormal > -0.2f;
	float Compression = 0.f;
	float CompressionRate = 0.f;
	if (!bAirborne)
	{
		const float DistanceUU = FVector::DotProduct(Ground.PlanePointUU - MountW, Normal) / AxisDotNormal;
		const float DistanceM = DistanceUU / ExoneerUnits::CmPerM;
		const float FreeLengthM = DistanceM - (Config.RadiusM - State.PrevRadialDropM);
		Compression = FMath::Clamp(Config.RestLengthM - FreeLengthM, 0.f, Config.TravelM + 0.05f);
		if (Compression <= 0.f)
		{
			bAirborne = true;
		}
		else
		{
			// Mount moving along the (downward) axis compresses the strut.
			CompressionRate = FVector::DotProduct(Body.PointVelocityUU(MountW), AxisW) / ExoneerUnits::CmPerM;
		}
	}

	if (bAirborne)
	{
		// No soil forces; the solver applies gravity. The wheel spins on the
		// motor curve and coasts down on brake + bearing drag.
		const float DriveTorque = MotorTorque(Config.StallTorqueNm, Config.NoLoadSpeedRadS, State.OmegaRadS, Command.Throttle, Command.SupplyFraction);
		State.OmegaRadS += Dt * DriveTorque / Inertia;
		const float BrakeTorque = Command.Brake * Config.MaxBrakeTorqueNm
			+ (Command.bParkingBrake ? Config.MaxBrakeTorqueNm : 0.f);
		State.OmegaRadS = ApplyDissipativeTorque(State.OmegaRadS, BrakeTorque + Config.BearingDragNm, Inertia, Dt);
		State.CompressionM = 0.f;
		State.PrevSlipAbs = 0.f;
		State.PrevRadialDropM = 0.f;

		Telemetry.OmegaRadS = State.OmegaRadS;
		Telemetry.SlipRatioAbs = 0.f;
		Telemetry.SinkageM = 0.f;
		Telemetry.CompressionM = 0.f;
		Telemetry.NormalLoadN = 0.f;
		Telemetry.DriveTorqueNm = DriveTorque;
		Telemetry.ElectricalPowerW = MotorElectricalPower(DriveTorque, State.OmegaRadS, Config.StallTorqueNm, Config.Efficiency, Config.CopperLossAtStallW);
		Telemetry.bInContact = false;
		return Out;
	}

	// --- Normal load from the strut (load transfer emerges from the body dynamics). ---
	const float StrutForce = SuspensionForce(Config.SpringNPerM, Config.DamperNSecPerM, Compression, CompressionRate, Config.TravelM, Config.BumpStopNPerM);
	const float NormalLoad = FMath::Max(0.f, StrutForce * -AxisDotNormal);
	State.CompressionM = Compression;

	// --- Regime-aware contact solve (rigid implicit Bekker vs flexible tire). ---
	const FWheelContactSolution Contact = SolveWheelContact(NormalLoad, Config.WidthM, Config.RadiusM,
		Command.TirePressurePa, Command.CarcassPressurePa, Ground.Soil, State.PrevSlipAbs, State.bPrevRigid);
	State.bPrevRigid = Contact.bRigid;
	// Low-pass the radial drop (~30 ms time constant at 120 Hz): soil failure
	// does not teleport, and the raw one-substep lag let sinkage flap with the
	// slip state, spiking the suspension into a visible bounce.
	const float TargetRadialDrop = Contact.bRigid ? Contact.SinkageM : Contact.DeflectionM;
	State.PrevRadialDropM = FMath::Lerp(State.PrevRadialDropM, TargetRadialDrop, 0.3f);
	const float EffectiveRadius = FMath::Max(Contact.EffectiveRadiusM, 0.05f);

	// --- Contact frame and slip. ---
	const FVector ForwardW = Body.BodyTM.TransformVectorNoScale(Ground.ForwardLocal);
	FVector LongitudinalDir = ForwardW - FVector::DotProduct(ForwardW, Normal) * Normal;
	if (!LongitudinalDir.Normalize(1e-4f))
	{
		return Out;   // wheel facing straight into the plane; degenerate, skip this substep
	}
	const FVector LateralDir = FVector::CrossProduct(Normal, LongitudinalDir);

	const float StaticRadius = FMath::Max(Config.RadiusM - State.PrevRadialDropM, 0.5f * Config.RadiusM);
	const FVector HubW = MountW + AxisW * (Config.RestLengthM - Compression) * ExoneerUnits::CmPerM;
	const FVector ContactUU = HubW - Normal * StaticRadius * ExoneerUnits::CmPerM;

	const FVector ContactVelocity = Body.PointVelocityUU(ContactUU) / ExoneerUnits::CmPerM;
	const float LongitudinalSpeed = FVector::DotProduct(ContactVelocity, LongitudinalDir);
	const float LateralSpeed = FVector::DotProduct(ContactVelocity, LateralDir);

	const float WheelSurfaceSpeed = State.OmegaRadS * EffectiveRadius;
	const float Slip = SlipRatio(WheelSurfaceSpeed, LongitudinalSpeed);
	const float Alpha = SlipAngle(LateralSpeed, LongitudinalSpeed);

	// The regularized ratio is right for the SHEAR FORCES but poisonous for
	// everything slower-acting: near standstill a barely-creeping wheel reads
	// ~100 percent slip, which inflated the slip-sinkage exponent (phantom
	// dig-in while parked) and made the governor cut launch torque the moment
	// the wheel began to turn - a bang-bang limit cycle that visibly shook
	// the rover. Physically, slip SINKAGE follows the soil excavation rate,
	// i.e. the slip VELOCITY |v_w - v_x|, so attenuate by it: a wheel truly
	// spinning against stuck ground still digs at full strength, a wheel
	// creeping at millimeters per second digs not at all.
	const float SlipVelocity = FMath::Abs(WheelSurfaceSpeed - LongitudinalSpeed);
	const float SlipActivity = FMath::Clamp(SlipVelocity / 0.3f, 0.f, 1.f);
	State.PrevSlipAbs = FMath::Abs(Slip) * SlipActivity;

	// --- Shear forces (one soil budget, friction ellipse emerges). ---
	const FShearForces Shear = CombinedShearForces(Contact.ContactAreaM2, NormalLoad, Slip, Alpha, Contact.PatchLengthM, Ground.Soil);

	// --- Motion resistance: compaction + bulldozing, tapered at rest so a parked rover never creeps. ---
	const float Compaction = CompactionResistance(Config.WidthM, Contact.SinkageM, Ground.Soil, State.PrevSlipAbs);
	const float Bulldozing = BulldozingResistance(Config.WidthM, Contact.SinkageM, Ground.Soil);
	const float ResistanceForce = -(Compaction + Bulldozing) * FMath::Clamp(LongitudinalSpeed / 0.1f, -1.f, 1.f);

	// --- Motor with the slip governor (talent lever: SlipCap < 1 holds the optimal window). ---
	float Governor = 1.f;
	if (Command.SlipCap < 0.999f)
	{
		Governor = SlipGovernor(State.PrevSlipAbs, Command.SlipCap * 0.667f, Command.SlipCap);
	}
	const float DriveTorque = MotorTorque(Config.StallTorqueNm, Config.NoLoadSpeedRadS, State.OmegaRadS, Command.Throttle, Command.SupplyFraction) * Governor;

	// --- Stiction: locked brakes at near-zero contact speed hold physically. ---
	// Static friction cancels residual creep, capped by BOTH the soil shear
	// budget and what the brake torque can react - never a constraint hack.
	FVector HoldForce = FVector::ZeroVector;
	const float BrakeTorqueMax = Command.Brake * Config.MaxBrakeTorqueNm
		+ (Command.bParkingBrake ? Config.MaxBrakeTorqueNm : 0.f);
	const float ContactSpeed = FMath::Sqrt(LongitudinalSpeed * LongitudinalSpeed + LateralSpeed * LateralSpeed);
	if (BrakeTorqueMax > 1.f && ContactSpeed < 0.05f && FMath::Abs(State.OmegaRadS * EffectiveRadius) < 0.05f)
	{
		const float HoldCap = FMath::Min(ShearBudget(Contact.ContactAreaM2, NormalLoad, Ground.Soil), BrakeTorqueMax / EffectiveRadius);
		const float HoldMagnitude = FMath::Min(ContactSpeed / 0.05f, 1.f) * HoldCap;
		if (ContactSpeed > 1e-4f)
		{
			const FVector CreepDir = (LongitudinalDir * LongitudinalSpeed + LateralDir * LateralSpeed) / ContactSpeed;
			HoldForce = -CreepDir * HoldMagnitude;
		}
	}

	// --- Wheel spin integration (semi-implicit, spec 5.4). ---
	const float OmegaBefore = State.OmegaRadS;
	// Explicit: drive torque and the ground reaction (soil shear loads the motor).
	State.OmegaRadS += Dt * (DriveTorque - Shear.LongitudinalN * EffectiveRadius) / Inertia;
	// Dissipative with no-reversal: brakes, internal rolling resistance, bearing drag.
	const float RollingResist = (Contact.bRigid ? Config.RollingResistRigid : Config.RollingResistFlexible) * NormalLoad * EffectiveRadius;
	State.OmegaRadS = ApplyDissipativeTorque(State.OmegaRadS, BrakeTorqueMax + RollingResist + Config.BearingDragNm, Inertia, Dt);
	// Slip-overshoot clamp: the explicit shear coupling must not oscillate
	// the surface speed across s = 0 when inertia is small.
	const float MaxSurfaceDelta = FMath::Max(2.f * FMath::Abs(OmegaBefore * EffectiveRadius - LongitudinalSpeed), 0.5f);
	State.OmegaRadS = FMath::Clamp(State.OmegaRadS,
		OmegaBefore - MaxSurfaceDelta / EffectiveRadius,
		OmegaBefore + MaxSurfaceDelta / EffectiveRadius);

	// --- Compose the body force (the single N -> UE conversion for this wheel). ---
	const FVector ForceN =
		Normal * NormalLoad
		+ LongitudinalDir * (Shear.LongitudinalN + ResistanceForce)
		+ LateralDir * Shear.LateralN
		+ HoldForce;
	Out.bApply = true;
	Out.ForceUE = ForceN * ExoneerUnits::NewtonsToUEForce;
	Out.LocationUU = ContactUU;

	// --- Telemetry. Copper loss is quadratic in torque, so a wheel stalled in
	// mud at full command burns CopperLossAtStallW as pure heat - that drain
	// is the intended survival pressure of bogging down.
	Telemetry.OmegaRadS = State.OmegaRadS;
	Telemetry.SlipRatioAbs = State.PrevSlipAbs;
	Telemetry.SinkageM = Contact.SinkageM;
	Telemetry.CompressionM = Compression;
	Telemetry.NormalLoadN = NormalLoad;
	Telemetry.DriveTorqueNm = DriveTorque;
	Telemetry.ElectricalPowerW = MotorElectricalPower(DriveTorque, State.OmegaRadS, Config.StallTorqueNm, Config.Efficiency, Config.CopperLossAtStallW);
	Telemetry.bInContact = true;
	return Out;
}

} // namespace ExoneerWheelSim
