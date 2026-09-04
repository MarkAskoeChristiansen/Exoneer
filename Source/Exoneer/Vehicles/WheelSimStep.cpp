// Copyright Exoneer contributors.
#include "Vehicles/WheelSimTypes.h"
#include "Vehicles/ExoneerVehicleUnits.h"
#include "Maintenance/ExoneerMaintenance.h"

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

	/** Filter time constant of the radial-drop low-pass (Lerp alpha at 120 Hz, ~28 ms). */
	constexpr float RadialDropFilterAlpha = 0.3f;
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
	// SOLVED compression drives the strut force and the contact patch; VISUAL
	// compression drives the drawn hub and the replicated quantum. They differ
	// by the sinkage, which is a rut nothing renders yet - see FWheelSimState.
	float SolvedCompression = 0.f;
	float VisualCompression = 0.f;
	float CompressionRate = 0.f;
	// The bump stop is part of the strut, so the geometry keeps tracking the
	// ground across it: the solved hub, the drawn hub and the bump-stop force
	// then agree. Past it the strut is genuinely out of travel.
	const float MaxCompression = Config.TravelM + FMath::Max(Config.BumpStopTravelM, 0.f);
	if (!bAirborne)
	{
		const float DistanceUU = FVector::DotProduct(Ground.PlanePointUU - MountW, Normal) / AxisDotNormal;
		const float DistanceM = DistanceUU / ExoneerUnits::CmPerM;
		const float SolvedFreeLengthM = DistanceM - (Config.RadiusM - State.PrevSolvedRadialDropM);
		SolvedCompression = FMath::Clamp(Config.RestLengthM - SolvedFreeLengthM, 0.f, MaxCompression);
		// Same geometry, tire squash only: the drawn wheel rests ON the
		// undisturbed surface instead of half sunk into it.
		const float VisualFreeLengthM = DistanceM - (Config.RadiusM - State.PrevVisualRadialDropM);
		VisualCompression = FMath::Clamp(Config.RestLengthM - VisualFreeLengthM, 0.f, MaxCompression);
		if (SolvedCompression <= 0.f)
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
		State.SolvedCompressionM = 0.f;
		// HOLD the contact state; do not invent a fresh one from its absence.
		// Soil does not un-compact and re-compact inside a frame: the rut the
		// wheel left is still there when it comes back down, and the tire is
		// still the shape its last load gave it. Zeroing these restarted the
		// radial-drop filter from a full-radius wheel on every touchdown, so
		// the wheel contacted early and then sank as the filter re-converged -
		// on the sand field a 69 mm ride-height pulse every time the probe
		// blinked, which is the chatter felt at slab edges. Held, the strut
		// force still starts from zero at first touch, so nothing steps; the
		// filter simply re-converges if the new ground is different.

		Telemetry.OmegaRadS = State.OmegaRadS;
		Telemetry.SlipRatioAbs = 0.f;
		Telemetry.SinkageM = 0.f;
		Telemetry.VisualCompressionM = 0.f;
		Telemetry.NormalLoadN = 0.f;
		Telemetry.ShearForceN = 0.f;
		Telemetry.SlipSpeedMS = 0.f;
		Telemetry.DriveTorqueNm = DriveTorque;
		Telemetry.ElectricalPowerW = MotorElectricalPower(DriveTorque, State.OmegaRadS, Config.StallTorqueNm, Config.Efficiency, Config.CopperLossAtStallW);
		Telemetry.LossPowerW = ExoneerMaintenance::MotorLossPowerW(DriveTorque, State.OmegaRadS, Config.StallTorqueNm,
			Config.Efficiency, Config.CopperLossAtStallW, Config.ControllerIdleDrawW);
		Telemetry.bInContact = false;
		return Out;
	}

	// --- Normal load from the strut (load transfer emerges from the body dynamics). ---
	const float StrutForce = SuspensionForce(Config.SpringNPerM, Config.DamperNSecPerM, SolvedCompression, CompressionRate, Config.TravelM, Config.BumpStopNPerM);
	const float NormalLoad = FMath::Max(0.f, StrutForce * -AxisDotNormal);
	State.SolvedCompressionM = SolvedCompression;

	// --- Which interface is this? ---
	// Soil and hard ground are two different physical contacts, and the model
	// must not charge one with the other's constants. On soil the tread
	// mobilises a fraction of the SOIL's own shear strength over the soil's
	// shear deformation modulus (centimetres). On rock or a metal deck there
	// is no soil to shear: tan(phi) already IS that surface's rubber friction
	// coefficient, so applying the soil-mobilisation factor to it double
	// counts, and the displacement that mobilises full friction is the
	// TREAD's own, a few millimetres. Substituting both here is what makes a
	// tire bite on firm ground at a handful of degrees of slip.
	FSoilParams Soil = Ground.Soil;
	float ShearScale = Config.TreadMobilisation;
	if (Soil.bCoulombInterface)
	{
		ShearScale = Config.HardSurfaceGrip;
		Soil.ShearK = FMath::Max(Config.TreadShearModulusM, 1e-4f);
		Soil.ShearKy = FMath::Max(Config.TreadShearModulusLateralM, 1e-4f);
	}

	// --- Regime-aware contact solve (rigid implicit Bekker vs flexible tire). ---
	const FWheelContactSolution Contact = SolveWheelContact(NormalLoad, Config.WidthM, Config.RadiusM,
		Command.TirePressurePa, Command.CarcassPressurePa, Soil, State.PrevSlipAbs, State.bPrevRigid);
	State.bPrevRigid = Contact.bRigid;
	// Low-pass both radial drops (~30 ms time constant at 120 Hz): soil failure
	// does not teleport, and the raw one-substep lag let sinkage flap with the
	// slip state, spiking the suspension into a visible bounce.
	//
	// SOLVED drop: a flexible tire is BOTH squashed and sitting in its rut -
	// the compaction and bulldozing terms below already charge it for the rut,
	// so riding on the undisturbed surface would be the model contradicting
	// itself. Everything mechanical below reads this one.
	const float TargetSolvedRadialDrop = Contact.bRigid
		? Contact.SinkageM
		: Contact.DeflectionM + Contact.SinkageM;
	// VISUAL drop: tire squash alone. The rut is a hole nothing draws, so
	// lowering the rendered hub into it just buries the wheel in an
	// undisturbed surface - on sand by roughly the full sinkage, about 53 mm,
	// which is the "wheel looks buried" the playtest reported. The visual will
	// fold sinkage back in when rut rendering exists.
	const float TargetVisualRadialDrop = Contact.bRigid ? 0.f : Contact.DeflectionM;
	State.PrevSolvedRadialDropM = FMath::Lerp(State.PrevSolvedRadialDropM, TargetSolvedRadialDrop, RadialDropFilterAlpha);
	State.PrevVisualRadialDropM = FMath::Lerp(State.PrevVisualRadialDropM, TargetVisualRadialDrop, RadialDropFilterAlpha);
	const float EffectiveRadius = FMath::Max(Contact.EffectiveRadiusM, 0.05f);

	// --- Contact frame and slip. ---
	const FVector ForwardW = Body.BodyTM.TransformVectorNoScale(Ground.ForwardLocal);
	FVector LongitudinalDir = ForwardW - FVector::DotProduct(ForwardW, Normal) * Normal;
	if (!LongitudinalDir.Normalize(1e-4f))
	{
		return Out;   // wheel facing straight into the plane; degenerate, skip this substep
	}
	const FVector LateralDir = FVector::CrossProduct(Normal, LongitudinalDir);

	const float StaticRadius = FMath::Max(Config.RadiusM - State.PrevSolvedRadialDropM, 0.5f * Config.RadiusM);
	const FVector HubW = MountW + AxisW * (Config.RestLengthM - SolvedCompression) * ExoneerUnits::CmPerM;
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
	const float SlipVelocity = WheelSurfaceSpeed - LongitudinalSpeed;   // signed
	const float SlipActivity = FMath::Clamp(FMath::Abs(SlipVelocity) / 0.3f, 0.f, 1.f);
	State.PrevSlipAbs = FMath::Abs(Slip) * SlipActivity;

	// --- Motion resistance: compaction + bulldozing, tapered at rest so a parked rover never creeps. ---
	const float Compaction = CompactionResistance(Config.WidthM, Contact.SinkageM, Soil, State.PrevSlipAbs);
	const float Bulldozing = BulldozingResistance(Config.WidthM, Contact.SinkageM, Soil);
	const float ResistanceForce = -(Compaction + Bulldozing) * FMath::Clamp(LongitudinalSpeed / 0.1f, -1.f, 1.f);

	// --- Motor with the slip governor (talent lever: SlipCap < 1 holds the optimal window). ---
	float Governor = 1.f;
	if (Command.SlipCap < 0.999f)
	{
		Governor = SlipGovernor(State.PrevSlipAbs, Command.SlipCap * 0.667f, Command.SlipCap);
	}
	const float DriveTorque = MotorTorque(Config.StallTorqueNm, Config.NoLoadSpeedRadS, State.OmegaRadS, Command.Throttle, Command.SupplyFraction) * Governor;

	// --- Tangential contact: Coulomb STICK below the sliding threshold, Janosi SLIDE above. ---
	// The Janosi law is a steady SLIDING law: F = Budget * E(u) with u built
	// from the slip, and E(u) -> u/2 as u -> 0. Used all the way down to zero
	// sliding speed it says a contact patch develops no force until it is
	// already sliding, which is simply not what a tire does - a parked rover
	// is held by STATIC friction. That missing branch is the whole of the
	// creep: every tangential term vanished linearly through zero, leaving a
	// lumped damper of ~500 kN/(m/s) that the 1/120 s substep cannot even
	// integrate (c*dt/m > 2, so it diverged into a buzz that rectified into
	// a slow walk). Both branches below are bounded by the SAME Mohr-Coulomb
	// budget, so nothing here can manufacture grip the soil does not have.
	const float ShearBudgetN = ShearBudget(Contact.ContactAreaM2, NormalLoad, Soil) * ShearScale;

	// Compliance the patch pushes against, in (m/s) of relative sliding per
	// N*s of tangential impulse. Longitudinally the impulse both accelerates
	// the body's share of the mass and spins the wheel back; laterally the
	// wheel has no degree of freedom, so only the body responds. The body half
	// is the true contact effective mass, which includes the pitch and roll the
	// force induces about the centre of mass.
	const float LongMassKg = FMath::Max(Body.ContactMassShareKg(ContactUU, LongitudinalDir), 1.f);
	const float LatMassKg = FMath::Max(Body.ContactMassShareKg(ContactUU, LateralDir), 1.f);
	const float LatCompliance = 1.f / LatMassKg;
	const float SpinCompliance = EffectiveRadius * EffectiveRadius / Inertia;
	const float LongCompliance = FMath::Max(1.f / LongMassKg + SpinCompliance, 1e-9f);

	// STICK: the tangential force that lands the patch at zero sliding speed
	// at the end of this substep, with the drive torque already accounted.
	// For a wheel rolling without slipping this reduces exactly to the
	// textbook T / (r * (1 + I/(m r^2))).
	const float StickLongN = (SlipVelocity / Dt + DriveTorque * EffectiveRadius / Inertia) / LongCompliance;
	const float StickLatN = -LateralSpeed / (Dt * LatCompliance);

	// SLIDE: the soil's own shear law, unchanged.
	FShearForces Shear = CombinedShearForces(Contact.ContactAreaM2, NormalLoad, Slip, Alpha, Contact.PatchLengthM, Soil);
	Shear.LongitudinalN *= ShearScale;
	Shear.LateralN *= ShearScale;
	Shear.ResultantN *= ShearScale;

	// A sliding force must dissipate: it may bring the sliding to a stop
	// within the substep but never drive it backwards. This is the tangential
	// twin of the wheel-spin overshoot clamp below, and it is an integrator
	// correction - it removes energy the explicit update was injecting, it
	// does not add damping.
	{
		const float MaxLongN = FMath::Abs(SlipVelocity) / (Dt * LongCompliance);
		const float MaxLatN = FMath::Abs(LateralSpeed) / (Dt * LatCompliance);
		float SlideScale = 1.f;
		if (FMath::Abs(Shear.LongitudinalN) > MaxLongN)
		{
			SlideScale = FMath::Min(SlideScale, MaxLongN / FMath::Max(FMath::Abs(Shear.LongitudinalN), 1e-4f));
		}
		if (FMath::Abs(Shear.LateralN) > MaxLatN)
		{
			SlideScale = FMath::Min(SlideScale, MaxLatN / FMath::Max(FMath::Abs(Shear.LateralN), 1e-4f));
		}
		Shear.LongitudinalN *= SlideScale;
		Shear.LateralN *= SlideScale;
	}

	// Coulomb friction is set-valued at zero sliding speed; StickSpeedMS is
	// the regularisation width over which the patch hands over from held to
	// sliding. Fully stuck below it, fully sliding above twice it.
	const float SlideSpeed = FMath::Sqrt(SlipVelocity * SlipVelocity + LateralSpeed * LateralSpeed);
	const float StickSpeed = FMath::Max(Config.StickSpeedMS, 1e-3f);
	const float StickWeight = 1.f - FMath::Clamp((SlideSpeed - StickSpeed) / StickSpeed, 0.f, 1.f);

	float TangentLongN = FMath::Lerp(Shear.LongitudinalN, StickLongN, StickWeight);
	float TangentLatN = FMath::Lerp(Shear.LateralN, StickLatN, StickWeight);

	// Mohr-Coulomb ceiling on the resultant: whatever the branch, the patch
	// can never carry more than the soil (or the interface) can shear. Past
	// it the wheel slips, which is the whole lesson of clay.
	const float TangentMag = FMath::Sqrt(TangentLongN * TangentLongN + TangentLatN * TangentLatN);
	if (TangentMag > ShearBudgetN && TangentMag > 1e-4f)
	{
		const float Limit = ShearBudgetN / TangentMag;
		TangentLongN *= Limit;
		TangentLatN *= Limit;
	}

	// --- Wheel spin integration (semi-implicit, spec 5.4). ---
	const float OmegaBefore = State.OmegaRadS;
	// Explicit: drive torque and the ground reaction of the force we actually
	// applied - stick and slide alike, so the two can never double count.
	State.OmegaRadS += Dt * (DriveTorque - TangentLongN * EffectiveRadius) / Inertia;
	// Dissipative with no-reversal: brakes, internal rolling resistance, bearing drag.
	const float BrakeTorqueMax = Command.Brake * Config.MaxBrakeTorqueNm
		+ (Command.bParkingBrake ? Config.MaxBrakeTorqueNm : 0.f);
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
		+ LongitudinalDir * (TangentLongN + ResistanceForce)
		+ LateralDir * TangentLatN;
	Out.bApply = true;
	Out.ForceUE = ForceN * ExoneerUnits::NewtonsToUEForce;
	Out.LocationUU = ContactUU;

	// --- Telemetry. Copper loss is quadratic in torque, so a wheel stalled in
	// mud at full command burns CopperLossAtStallW as pure heat - that drain
	// is the intended survival pressure of bogging down.
	Telemetry.OmegaRadS = State.OmegaRadS;
	Telemetry.SlipRatioAbs = State.PrevSlipAbs;
	Telemetry.SinkageM = Contact.SinkageM;
	Telemetry.VisualCompressionM = VisualCompression;
	Telemetry.NormalLoadN = NormalLoad;
	// Tread wear is abrasion, so it is paid for in frictional WORK: the force
	// the patch carried times how fast it slid. Both halves are marshaled.
	Telemetry.ShearForceN = FMath::Sqrt(TangentLongN * TangentLongN + TangentLatN * TangentLatN);
	Telemetry.SlipSpeedMS = SlideSpeed;
	Telemetry.DriveTorqueNm = DriveTorque;
	Telemetry.ElectricalPowerW = MotorElectricalPower(DriveTorque, State.OmegaRadS, Config.StallTorqueNm, Config.Efficiency, Config.CopperLossAtStallW);
	// Heat is the loss half of that draw only; the rest left as shaft work.
	Telemetry.LossPowerW = ExoneerMaintenance::MotorLossPowerW(DriveTorque, State.OmegaRadS, Config.StallTorqueNm,
		Config.Efficiency, Config.CopperLossAtStallW, Config.ControllerIdleDrawW);
	Telemetry.bInContact = true;
	return Out;
}

} // namespace ExoneerWheelSim
