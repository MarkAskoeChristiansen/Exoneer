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
#include "PhysicalMaterials/PhysicalMaterial.h"

void UWheelModule::Initialize(AVehicleConstruct* InConstruct, int32 InBlockInstanceId)
{
	Super::Initialize(InConstruct, InBlockInstanceId);

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

	// CTIS valve: a physical pump rate, not a set-instantly value.
	if (CtisPumpDirection != 0)
	{
		TirePressureKPa = FMath::Clamp(
			TirePressureKPa + (float)CtisPumpDirection * Spec->CtisRateKPaPerS * DeltaSeconds,
			Spec->MinTirePressureKPa, Spec->MaxTirePressureKPa);
	}

	// Steering servo: slew toward the Ackermann target at the authored rate.
	const float MaxSteer = FMath::DegreesToRadians(Spec->MaxSteerAngleDeg);
	const float SteerTarget = FMath::Clamp(TargetSteerAngleRad + SteerTrimRad, -MaxSteer, MaxSteer);
	const float SteerRate = FMath::DegreesToRadians(Spec->SteerRateDegPerS) * DeltaSeconds;
	SteerAngleRad += FMath::Clamp(SteerTarget - SteerAngleRad, -SteerRate, SteerRate);

	// Suspension probe (game thread only - scene queries are forbidden on the
	// physics thread; substeps re-solve against this cached plane).
	const FTransform BlockWorld = Owner->GetBlockWorldTransform(*Record);
	const FVector Start = BlockWorld.GetLocation();
	const FVector AxisWorld = -BlockWorld.GetUnitAxis(EAxis::Z);
	const float RayLengthUU = (Spec->RestLengthM + Spec->TravelM + Spec->RadiusM) * ExoneerUnits::CmPerM + 5.f;

	FCollisionQueryParams Params(SCENE_QUERY_STAT(ExoneerWheelProbe), /*bTraceComplex*/ false, Owner);
	Params.bReturnPhysicalMaterial = true;
	FHitResult Hit;
	const bool bHit = Owner->GetWorld()->LineTraceSingleByChannel(
		Hit, Start, Start + AxisWorld * RayLengthUU, ECC_WheelProbe, Params);

	const FTransform BlockLocal = Owner->GetBlockLocalTransform(*Record);
	const FQuat BlockLocalQuat = BlockLocal.GetRotation();
	Ground.bHasContact = bHit;
	Ground.PlanePointUU = bHit ? Hit.ImpactPoint : FVector::ZeroVector;
	Ground.PlaneNormal = bHit ? Hit.ImpactNormal : FVector::UpVector;
	Ground.Soil = bHit ? ResolveSoil(Hit) : ExoneerTerramechanics::FirmGroundDefault();
	Ground.MountLocalUU = BlockLocal.GetLocation();
	Ground.AxisLocal = BlockLocalQuat.RotateVector(-FVector::ZAxisVector);
	Ground.ForwardLocal = BlockLocalQuat.RotateVector(
		FQuat(FVector::ZAxisVector, SteerAngleRad).RotateVector(FVector::XAxisVector));
	bGroundCacheValid = true;

	// Publish quantized state under deadbands (steer 0.01 rad, omega 0.25
	// rad/s, one quantum elsewhere) - dashboard and animation data, kept off
	// the block records so it never triggers a client visual rebuild.
	FVehicleWheelStateItem& Item = Owner->FindOrAddWheelStateItem(GetBlockInstanceId());
	const int16 SteerQ = (int16)FMath::Clamp(FMath::RoundToInt(SteerAngleRad * 1000.f), -32000, 32000);
	const int16 OmegaQ = (int16)FMath::Clamp(FMath::RoundToInt(LastTelemetry.OmegaRadS * 64.f), -32000, 32000);
	const uint8 SlipQ = (uint8)FMath::Clamp(FMath::RoundToInt(LastTelemetry.SlipRatioAbs * 255.f), 0, 255);
	const uint8 SinkageQ = (uint8)FMath::Clamp(FMath::RoundToInt(LastTelemetry.SinkageM * 100.f), 0, 255);
	const uint8 CompressionQ = (uint8)FMath::Clamp(FMath::RoundToInt(LastTelemetry.CompressionM / FMath::Max(Spec->TravelM, 0.01f) * 255.f), 0, 255);
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

	ExoneerWheelSim::FWheelSimConfig& Config = OutItem.Config;
	Config.RadiusM = Spec->RadiusM;
	Config.WidthM = Spec->WidthM;
	Config.SpringNPerM = Spec->SpringRateNPerM;
	Config.DamperNSecPerM = Spec->DamperNSecPerM;
	Config.RestLengthM = Spec->RestLengthM;
	Config.TravelM = Spec->TravelM;
	Config.BumpStopNPerM = Spec->BumpStopNPerM;
	Config.WheelInertiaKgM2 = WheelInertiaKgM2;
	Config.StallTorqueNm = Spec->bDriven ? Spec->MaxMotorTorqueNm : 0.f;
	Config.NoLoadSpeedRadS = Spec->NoLoadSpeedRadS;
	Config.Efficiency = Spec->DrivetrainEfficiency;
	Config.CopperLossAtStallW = Spec->CopperLossAtStallW;
	Config.MaxBrakeTorqueNm = Spec->MaxBrakeTorqueNm;
	Config.RollingResistRigid = Spec->RollingResistRigid;
	Config.RollingResistFlexible = Spec->RollingResistFlexible;
	Config.BearingDragNm = Spec->BearingDragNm;

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
