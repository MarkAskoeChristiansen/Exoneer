// Copyright Exoneer contributors.
#include "Vehicles/WheelModule.h"
#include "Exoneer.h"
#include "Vehicles/VehicleConstruct.h"
#include "Vehicles/ExoneerVehicleUnits.h"
#include "Data/VehicleBlockDefinitionDataAsset.h"
#include "Physics/ExoneerSoilPhysicalMaterial.h"
#include "World/PlanetEnvironmentManager.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Maintenance/ExoneerMaintenance.h"
#include "Building/BasePiece.h"

void UWheelModule::Initialize(AVehicleConstruct* InConstruct, int32 InBlockInstanceId)
{
	Super::Initialize(InConstruct, InBlockInstanceId);

	// Deadband baselines start at the record's own readings (a loaded save can
	// bring back a hot, worn wheel), so the first accumulated 1 C or 0.1 mm of
	// change is measured from what clients were last told.
	if (const FVehicleBlockRecord* Record = FindRecord())
	{
		LastMarkedTreadDepthMm = Record->Condition.TreadDepthMm;
		LastMarkedWindingTempC = Record->Condition.WindingTempC;
	}

	const UVehicleBlockDefinitionDataAsset* Def = FindDef();
	const FVehicleWheelSpec* Spec = FindSpec();
	if (!Def || !Spec)
	{
		return;
	}

	// I_w: authored override, or 0.6 * m * r^2 - between solid cylinder (0.5)
	// and hoop (1.0), because tire mass sits at the rim.
	WheelInertiaKgM2 = Spec->WheelInertiaOverrideKgM2 > 0.f
		? Spec->WheelInertiaOverrideKgM2
		: 0.6f * FMath::Max(Def->Mass, 1.f) * FMath::Square(Spec->RadiusM);

	// Persistent settings: saved values when the construct holds a pending
	// restore for this block, otherwise the authored nominal.
	AVehicleConstruct::FWheelSavedState Saved;
	if (InConstruct && InConstruct->TakeSavedWheelState(InBlockInstanceId, Saved) && Saved.TirePressureKPa > 0.f)
	{
		TirePressureKPa = Saved.TirePressureKPa;
		SteerTrimRad = FMath::DegreesToRadians(Saved.SteerTrimDeg);
	}
	else
	{
		TirePressureKPa = Spec->NominalTirePressureKPa;
		SteerTrimRad = 0.f;
	}
	TirePressureKPa = FMath::Clamp(TirePressureKPa, Spec->MinTirePressureKPa, Spec->MaxTirePressureKPa);

	// Suspension stability bounds (spec 6.3) at the configured 1/120 substep:
	// a bad authored value must be a logged content error, not an explosion.
	constexpr float SubstepDt = 1.f / 120.f;
	if (InConstruct && InConstruct->PhysicsRoot && InConstruct->PhysicsRoot->IsSimulatingPhysics())
	{
		const float TotalMass = InConstruct->PhysicsRoot->GetMass();
		const float CornerMass = FMath::Max(TotalMass / 4.f, 25.f);
		const float SpringBound = 0.25f * CornerMass / (SubstepDt * SubstepDt);
		const float DamperBound = CornerMass / SubstepDt;
		if (Spec->SpringRateNPerM > SpringBound || Spec->DamperNSecPerM > DamperBound)
		{
			UE_LOG(LogExoneer, Warning,
				TEXT("%s wheel %d: suspension exceeds stability bounds at %.0f Hz substeps (spring %.0f > %.0f or damper %.0f > %.0f for ~%.0f kg corner mass)."),
				*InConstruct->GetName(), InBlockInstanceId, 1.f / SubstepDt,
				Spec->SpringRateNPerM, SpringBound, Spec->DamperNSecPerM, DamperBound, CornerMass);
		}
	}

	// Seed the replicated side-array entry so clients can pose the wheel.
	if (InConstruct)
	{
		FVehicleWheelStateItem& Item = InConstruct->FindOrAddWheelStateItem(InBlockInstanceId);
		Item.TirePressureQ = (uint8)FMath::Clamp(FMath::RoundToInt(TirePressureKPa / 2.f), 0, 255);
		InConstruct->WheelStates.MarkItemDirty(Item);
	}
}

void UWheelModule::Shutdown()
{
	bShutDown = true;
	if (AVehicleConstruct* Owner = GetConstruct())
	{
		Owner->RemoveWheelStateItem(GetBlockInstanceId());
	}
}

void UWheelModule::TickModule(float DeltaSeconds)
{
	AVehicleConstruct* Owner = GetConstruct();
	const FVehicleBlockRecord* Record = FindRecord();
	const FVehicleWheelSpec* Spec = FindSpec();
	bGroundCacheValid = false;
	if (bShutDown || !Owner || !Record || !Spec || DeltaSeconds <= 0.f)
	{
		return;
	}

	// Steering servo: slew toward the Ackermann target at the authored rate.
	const float MaxSteer = FMath::DegreesToRadians(Spec->MaxSteerAngleDeg);
	const float SteerTarget = FMath::Clamp(TargetSteerAngleRad + SteerTrimRad, -MaxSteer, MaxSteer);
	const float SteerRate = FMath::DegreesToRadians(Spec->SteerRateDegPerS) * DeltaSeconds;
	SteerAngleRad += FMath::Clamp(SteerTarget - SteerAngleRad, -SteerRate, SteerRate);

	// Suspension probe (game thread only - scene queries are forbidden on the
	// physics thread; substeps re-solve against this cached plane). Start the
	// query a wheel radius ABOVE the block: a deeply sunk wheel would otherwise
	// begin it inside the terrain collision, which registers no hit - contact
	// flickers off, forces vanish, the body drops, and the wheels jitter (the
	// "2/4 wheels" dropout).
	//
	// The query is a SWEPT SPHERE of the wheel's own radius, not a line. A line
	// models a knife-edge wheel: at a curb the traced distance stepped by the
	// full curb height in the single frame the ray crossed the lip, and the
	// strut turned that step straight into force - a 25 kN spike, nine times
	// the corner load, that no damper could soften because the damper only sees
	// the body's own speed, never the rate of change of the traced distance. A
	// round wheel touches a curb at its TANGENT point, so the sphere climbs the
	// same curb continuously over a radius of travel. This is the geometrically
	// correct query for the shape being simulated, not a filter over a wrong one.
	const FTransform BlockWorld = Owner->GetBlockWorldTransform(*Record);
	const FVector AxisWorld = -BlockWorld.GetUnitAxis(EAxis::Z);
	const float StartOffsetUU = Spec->RadiusM * ExoneerUnits::CmPerM;
	const FVector Start = BlockWorld.GetLocation() - AxisWorld * StartOffsetUU;
	const float RayLengthUU = (Spec->RestLengthM + Spec->TravelM + Spec->RadiusM) * ExoneerUnits::CmPerM + StartOffsetUU + 5.f;
	const float SweepLengthUU = (Spec->RadiusM + Spec->RestLengthM + Spec->TravelM + Spec->BumpStopTravelM)
		* ExoneerUnits::CmPerM + 5.f;

	FCollisionQueryParams Params(SCENE_QUERY_STAT(ExoneerWheelProbe), /*bTraceComplex*/ false, Owner);
	Params.bReturnPhysicalMaterial = true;

	// A pawn is never ground. The player capsule already ignores this channel;
	// this guard keeps any future pawn from becoming a springboard (standing
	// beside a wheel would otherwise read as instant full compression).
	auto IsUsableGround = [&AxisWorld](const FHitResult& Candidate, bool bFound)
	{
		if (!bFound || Candidate.bStartPenetrating)
		{
			return false;
		}
		if (Candidate.GetActor() && Candidate.GetActor()->IsA<APawn>())
		{
			return false;
		}
		// A face the strut cannot press into is a wall, not a floor. Same
		// threshold the sim uses, so the probe never hands over a plane the
		// substep would immediately discard.
		return FVector::DotProduct(AxisWorld, Candidate.ImpactNormal) <= -0.2f;
	};

	FHitResult Hit;
	const bool bSweptFound = Owner->GetWorld()->SweepSingleByChannel(
		Hit, Start, Start + AxisWorld * SweepLengthUU, FQuat::Identity, ECC_WheelProbe,
		FCollisionShape::MakeSphere(StartOffsetUU), Params);
	bool bHit = IsUsableGround(Hit, bSweptFound);
	if (!bHit)
	{
		// Fallbacks the sphere cannot serve: it started inside geometry, or it
		// caught a wall shoulder instead of the floor under the hub.
		FHitResult LineHit;
		const bool bLineFound = Owner->GetWorld()->LineTraceSingleByChannel(
			LineHit, Start, Start + AxisWorld * RayLengthUU, ECC_WheelProbe, Params);
		if (IsUsableGround(LineHit, bLineFound))
		{
			Hit = LineHit;
			bHit = true;
		}
	}

	const FTransform BlockLocal = Owner->GetBlockLocalTransform(*Record);
	const FQuat BlockLocalQuat = BlockLocal.GetRotation();
	if (bHit)
	{
		// Which piece the wheel stands on. This is the only moment the query's
		// actor is in scope - the marshaled ground struct keeps geometry and
		// soil only - so the link is captured here and spent on the game thread.
		GroundPiece = Cast<ABasePiece>(Hit.GetActor());
		Ground.bHasContact = true;
		Ground.PlanePointUU = Hit.ImpactPoint;
		Ground.PlaneNormal = Hit.ImpactNormal;
		Ground.Soil = ResolveSoil(Hit);
		ContactGraceRemaining = FMath::Max(Spec->ContactGraceSeconds, 0.f);
	}
	else
	{
		// Hold the last plane briefly rather than deleting the whole corner
		// load for a frame. Losing two wheels for one frame on a 1.85 t rover
		// removes about a third of a g plus the roll moment of the missing
		// corner, which is the jump felt at slab edges.
		ContactGraceRemaining = FMath::Max(ContactGraceRemaining - DeltaSeconds, 0.f);
		if (ContactGraceRemaining <= 0.f)
		{
			GroundPiece = nullptr;
			Ground.bHasContact = false;
			Ground.PlanePointUU = FVector::ZeroVector;
			Ground.PlaneNormal = FVector::UpVector;
			Ground.Soil = ExoneerTerramechanics::FirmGroundDefault();
		}
	}
	Ground.MountLocalUU = BlockLocal.GetLocation();
	Ground.AxisLocal = BlockLocalQuat.RotateVector(-FVector::ZAxisVector);
	Ground.ForwardLocal = BlockLocalQuat.RotateVector(
		FQuat(FVector::ZAxisVector, SteerAngleRad).RotateVector(FVector::XAxisVector));
	bGroundCacheValid = true;

	// Condition integration lives HERE and not in ConsumeTelemetry: the module
	// ticks every frame, while telemetry only arrives while the Chaos body is
	// Dynamic. A construct that trips its cutout and parks sleeps by design, so
	// integrating on telemetry left the winding frozen at its trip temperature
	// and the latch never cleared - a terminal state that stopped recovering.
	if (Owner->HasAuthority())
	{
		if (FVehicleBlockRecord* MutableRecord = Owner->FindMutableRecord(GetBlockInstanceId()))
		{
			IntegrateCondition(*MutableRecord, *Spec, DeltaSeconds);
		}
		ReportGroundLoad();
		// Consumed (or dropped with the record): this frame's packets are spent.
		FrameLossWSum = 0.f;
		FrameSlipSum = 0.f;
		FrameLoadNSum = 0.f;
		FrameShearPowerWSum = 0.f;
		FrameTelemetryCount = 0;
		FrameContactCount = 0;
	}

	// Publish quantized state under deadbands (steer 0.01 rad, omega 0.25
	// rad/s, one quantum elsewhere) - dashboard and animation data, kept off
	// the block records so it never triggers a client visual rebuild.
	FVehicleWheelStateItem& Item = Owner->FindOrAddWheelStateItem(GetBlockInstanceId());
	const int16 SteerQ = (int16)FMath::Clamp(FMath::RoundToInt(SteerAngleRad * 1000.f), -32000, 32000);
	const int16 OmegaQ = (int16)FMath::Clamp(FMath::RoundToInt(LastTelemetry.OmegaRadS * 64.f), -32000, 32000);
	const uint8 SlipQ = (uint8)FMath::Clamp(FMath::RoundToInt(LastTelemetry.SlipRatioAbs * 255.f), 0, 255);
	const uint8 SinkageQ = (uint8)FMath::Clamp(FMath::RoundToInt(LastTelemetry.SinkageM * 100.f), 0, 255);
	// Denominator is the strut's WHOLE range, bump stop included - the same
	// range the solver clamps compression to. Quantizing against TravelM alone
	// saturated at 255 while the solver kept compressing, so the mesh was drawn
	// with up to 50 mm more hub drop than the physics used and sank into the
	// floor at exactly the moments that matter (curbs, hard landings).
	const float VisualTravelM = FMath::Max(Spec->TravelM + Spec->BumpStopTravelM, 0.01f);
	const uint8 CompressionQ = (uint8)FMath::Clamp(FMath::RoundToInt(LastTelemetry.VisualCompressionM / VisualTravelM * 255.f), 0, 255);
	const uint8 PressureQ = (uint8)FMath::Clamp(FMath::RoundToInt(TirePressureKPa / 2.f), 0, 255);
	const bool bChanged =
		FMath::Abs(Item.SteerAngleQ - SteerQ) >= 10
		|| FMath::Abs(Item.OmegaQ - OmegaQ) >= 16
		|| (Item.OmegaQ != 0) != (OmegaQ != 0)
		|| Item.SlipQ != SlipQ
		|| Item.SinkageQ != SinkageQ
		|| Item.CompressionQ != CompressionQ
		|| Item.TirePressureQ != PressureQ;
	if (bChanged)
	{
		Item.SteerAngleQ = SteerQ;
		Item.OmegaQ = OmegaQ;
		Item.SlipQ = SlipQ;
		Item.SinkageQ = SinkageQ;
		Item.CompressionQ = CompressionQ;
		Item.TirePressureQ = PressureQ;
		Owner->WheelStates.MarkItemDirty(Item);
	}
}

float UWheelModule::GetCurrentDraw() const
{
	// Physical electrical model: mechanical power over efficiency plus copper
	// loss quadratic in torque (from the substep telemetry), plus the motor
	// controller's constant idle draw. No rating cap - a stalled motor in mud
	// burns its full copper loss, which is the intended battery pressure.
	const FVehicleWheelSpec* Spec = FindSpec();
	const float IdleDraw = Spec ? Spec->ControllerIdleDrawW : 0.f;
	return FMath::Max(0.f, LastTelemetry.ElectricalPowerW) + IdleDraw;
}

void UWheelModule::RestorePersistentState(float InTirePressureKPa, float InSteerTrimDeg)
{
	if (const FVehicleWheelSpec* Spec = FindSpec())
	{
		if (InTirePressureKPa > 0.f)
		{
			TirePressureKPa = FMath::Clamp(InTirePressureKPa, Spec->MinTirePressureKPa, Spec->MaxTirePressureKPa);
		}
		SteerTrimRad = FMath::DegreesToRadians(InSteerTrimDeg);
	}
}

bool UWheelModule::BuildSimInput(ExoneerWheelSim::FWheelSimInputItem& OutItem) const
{
	const AVehicleConstruct* Owner = GetConstruct();
	const UVehicleBlockDefinitionDataAsset* Def = FindDef();
	const FVehicleWheelSpec* Spec = FindSpec();
	if (bShutDown || !Owner || !Def || !Spec || !bGroundCacheValid)
	{
		return false;
	}

	OutItem.BlockInstanceId = GetBlockInstanceId();

	// Condition-driven capacity scales, resolved on the GAME thread and baked
	// into the plain Config: the sim callback may not touch a UObject.
	const FVehicleBlockRecord* Rec = FindRecord();
	float TreadScale = 1.f;
	float ThermalScale = 1.f;
	bool bThermalCutout = false;
	if (Rec)
	{
		TreadScale = ExoneerMaintenance::TreadMobilisationScale(Rec->Condition.TreadDepthMm, Spec->NewTreadDepthMm);
		ThermalScale = ExoneerMaintenance::ThermalDerateScale(
			Rec->Condition.WindingTempC, Spec->DerateOnsetTempC, Spec->TripTempC);
		bThermalCutout = Rec->Condition.bThermalCutout != 0;
	}

	ExoneerWheelSim::FWheelSimConfig& Config = OutItem.Config;
	Config.RadiusM = Spec->RadiusM;
	Config.WidthM = Spec->WidthM;
	Config.SpringNPerM = Spec->SpringRateNPerM;
	Config.DamperNSecPerM = Spec->DamperNSecPerM;
	Config.RestLengthM = Spec->RestLengthM;
	Config.TravelM = Spec->TravelM;
	Config.BumpStopNPerM = Spec->BumpStopNPerM;
	Config.BumpStopTravelM = Spec->BumpStopTravelM;
	Config.WheelInertiaKgM2 = WheelInertiaKgM2;
	// A cut-out motor freewheels: MotorTorque returns 0 for a zero stall
	// torque and MotorElectricalPower guards the same denominator, so the
	// wheel keeps rolling and its heat input drops to the controller draw.
	Config.StallTorqueNm = (Spec->bDriven && !bThermalCutout ? Spec->MaxMotorTorqueNm : 0.f) * ThermalScale;
	Config.NoLoadSpeedRadS = Spec->NoLoadSpeedRadS;
	Config.Efficiency = Spec->DrivetrainEfficiency;
	Config.CopperLossAtStallW = Spec->CopperLossAtStallW;
	Config.ControllerIdleDrawW = Spec->ControllerIdleDrawW;
	Config.MaxBrakeTorqueNm = Spec->MaxBrakeTorqueNm;
	Config.RollingResistRigid = Spec->RollingResistRigid;
	Config.RollingResistFlexible = Spec->RollingResistFlexible;
	Config.BearingDragNm = Spec->BearingDragNm;
	Config.TreadMobilisation = Spec->TreadMobilisation * TreadScale;
	// Wear spends BOTH interfaces: a bald tire mobilises less soil shear and
	// lays down less rubber on rock.
	Config.HardSurfaceGrip = Spec->HardSurfaceGrip * TreadScale;
	Config.TreadShearModulusM = Spec->TreadShearModulusM;
	Config.TreadShearModulusLateralM = Spec->TreadShearModulusLateralM;
	Config.StickSpeedMS = Spec->StickSpeedMS;

	ExoneerWheelSim::FWheelSimCommand& Command = OutItem.Command;
	Command.Throttle = ThrottleCommand;
	Command.Brake = BrakeCommand;
	Command.bParkingBrake = bParkingBrake;
	Command.SlipCap = TargetSlipCap;
	Command.TirePressurePa = TirePressureKPa * 1000.f;
	Command.CarcassPressurePa = Spec->CarcassStiffnessKPa * 1000.f;
	Command.SupplyFraction = Owner->PowerSupplyFraction;

	OutItem.Ground = Ground;
	return true;
}

void UWheelModule::ConsumeTelemetry(const ExoneerWheelSim::FWheelSimTelemetry& Telemetry)
{
	LastTelemetry = Telemetry;

	// Accumulate, never integrate: the construct pops one packet per physics
	// substep, so any dt used here would be counted several times a frame.
	// TickModule spends these against the real frame dt.
	FrameLossWSum += FMath::Max(Telemetry.LossPowerW, 0.f);
	++FrameTelemetryCount;
	if (Telemetry.bInContact)
	{
		FrameSlipSum += FMath::Clamp(Telemetry.SlipRatioAbs, 0.f, 1.f);
		FrameLoadNSum += FMath::Max(Telemetry.NormalLoadN, 0.f);
		FrameShearPowerWSum += FMath::Max(Telemetry.ShearForceN, 0.f) * FMath::Max(Telemetry.SlipSpeedMS, 0.f);
		++FrameContactCount;
	}
}

/**
 * SERVER. Attribute this wheel's load to the piece it is standing on, ONCE per
 * frame. Reporting per telemetry packet instead would multiply what a deck
 * sees by the substep count (about two at 120 Hz substeps and 60 fps), and a
 * sleeping body sends no packets at all - the same two traps that moved
 * condition integration out of ConsumeTelemetry.
 */
void UWheelModule::ReportGroundLoad()
{
	// No contact this frame means no force on anything, whatever the probe
	// found: an airborne wheel over a deck presses on nothing.
	if (FrameContactCount <= 0)
	{
		return;
	}
	if (ABasePiece* Piece = GroundPiece.Get())
	{
		// The mean load while in contact, which is the force this piece felt
		// through the wheel. ReportLiveLoad refuses ghosts and half-welded
		// pieces itself.
		Piece->ReportLiveLoad(FrameLoadNSum / static_cast<float>(FrameContactCount));
	}
}

void UWheelModule::IntegrateCondition(FVehicleBlockRecord& Record, const FVehicleWheelSpec& Spec, float DeltaSeconds)
{
	AVehicleConstruct* Owner = GetConstruct();
	if (!Owner)
	{
		return;
	}
	const float Dt = FMath::Clamp(DeltaSeconds, 0.001f, 0.05f);
	const bool bWasCutout = Record.Condition.bThermalCutout != 0;

	// Tread only wears where rubber meets ground, and only for the share of the
	// frame the wheel was actually on the ground.
	if (FrameContactCount > 0 && FrameTelemetryCount > 0 && Record.Condition.HasTire())
	{
		// Abrasion is an ENERGY process: rubber leaves the tread in proportion
		// to the frictional work done in the patch. The substeps report the
		// force the patch carried and how fast it slid, so the frame's work is
		// their mean product over the share of the frame the wheel was down.
		const float MeanShearPowerW = FrameShearPowerWSum / (float)FrameContactCount;
		const float ContactDt = Dt * (float)FrameContactCount / (float)FrameTelemetryCount;
		const float Wear = ExoneerMaintenance::TreadWearMm(MeanShearPowerW * ContactDt);
		if (Wear > 0.f)
		{
			Record.Condition.TreadDepthMm = FMath::Max(0.f, Record.Condition.TreadDepthMm - Wear);
		}
	}

	// The winding heats and cools whatever the wheel is doing, so this runs
	// airborne, on a bare rim, and while the body sleeps. With no packets this
	// frame the only loss left is the controller's own electronics draw, which
	// equilibrates less than a degree above ambient. Ambient comes from the
	// biome day/night curve; 20 C when the map has no environment manager.
	const float LossW = FrameTelemetryCount > 0
		? FrameLossWSum / (float)FrameTelemetryCount
		: FMath::Max(Spec.ControllerIdleDrawW, 0.f);
	float AmbientC = 20.f;
	if (const APlanetEnvironmentManager* Environment = Owner->GetEnvironmentManager())
	{
		AmbientC = Environment->GetCurrentAmbientTemperatureC();
	}
	Record.Condition.WindingTempC = FMath::Clamp(
		ExoneerMaintenance::WindingTempStep(Record.Condition.WindingTempC, LossW,
			AmbientC, Spec.CoolingWPerC, Spec.ThermalMassJPerC, Dt),
		AmbientC - 5.f, Spec.TripTempC + 60.f);
	Record.Condition.bThermalCutout = ExoneerMaintenance::ThermalCutoutLatch(
		bWasCutout, Record.Condition.WindingTempC, Spec.TripTempC, Spec.CutoutClearTempC) ? 1 : 0;

	// Deadbands (0.1 mm, 1 C) against the values clients last received, so the
	// record goes dirty once per accumulated step and not per frame. The cutout
	// flip is a state change and always goes out.
	if (FMath::Abs(LastMarkedTreadDepthMm - Record.Condition.TreadDepthMm) >= 0.1f
		|| FMath::Abs(LastMarkedWindingTempC - Record.Condition.WindingTempC) >= 1.f
		|| bWasCutout != (Record.Condition.bThermalCutout != 0))
	{
		LastMarkedTreadDepthMm = Record.Condition.TreadDepthMm;
		LastMarkedWindingTempC = Record.Condition.WindingTempC;
		Owner->MarkRecordDirty(Record);
	}
}

const FVehicleWheelSpec* UWheelModule::FindSpec() const
{
	const UVehicleBlockDefinitionDataAsset* Def = FindDef();
	return Def && Def->bIsWheel ? &Def->WheelSpec : nullptr;
}

ExoneerTerramechanics::FSoilParams UWheelModule::ResolveSoil(const FHitResult& Hit) const
{
	// 1. The hit surface's own soil physical material is authoritative.
	if (const UExoneerSoilPhysicalMaterial* SoilMaterial = Cast<UExoneerSoilPhysicalMaterial>(Hit.PhysMaterial.Get()))
	{
		return SoilMaterial->ToSoilParams();
	}
	// 2. Biome default (a soft-soil planet can make everything mud).
	if (const AVehicleConstruct* Owner = GetConstruct())
	{
		if (const APlanetEnvironmentManager* Environment = Owner->GetEnvironmentManager())
		{
			if (const UExoneerSoilPhysicalMaterial* Default = Environment->GetDefaultSoil())
			{
				return Default->ToSoilParams();
			}
		}
	}
	// 3. Near-rigid ground; traction Coulomb-limited by the plain material's friction.
	const float Friction = Hit.PhysMaterial.IsValid() ? Hit.PhysMaterial->Friction : 0.7f;
	return ExoneerTerramechanics::FirmGroundDefault(Friction);
}
