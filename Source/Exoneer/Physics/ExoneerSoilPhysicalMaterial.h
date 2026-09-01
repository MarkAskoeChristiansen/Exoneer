// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Vehicles/ExoneerTerramechanics.h"
#include "ExoneerSoilPhysicalMaterial.generated.h"

/**
 * A substrate for wheel-terrain interaction. Assign to the PhysMaterial slot
 * of a ground material (the material instance must also set
 * bOverridePhysMaterial); the wheel probe resolves it from the hit result.
 *
 * Authored in the units the terramechanics literature publishes (kN, kPa,
 * degrees - Wong's tables paste in directly); ToSoilParams() converts to SI
 * exactly once. Defaults are Wong's LLL dry sand so an unauthored asset is
 * still physical.
 */
UCLASS(BlueprintType)
class EXONEER_API UExoneerSoilPhysicalMaterial : public UPhysicalMaterial
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Soil")
	FText SoilDisplayName;

	/** Bekker cohesive sinkage modulus k_c [kN/m^(n+1)]. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Soil")
	float BekkerKc = 0.99f;

	/** Bekker frictional sinkage modulus k_phi [kN/m^(n+2)]. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Soil")
	float BekkerKphi = 1528.43f;

	/** Sinkage exponent n at zero slip. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Soil", meta = (ClampMin = "0.2", ClampMax = "2.5"))
	float BekkerN = 1.1f;

	/** Slip-sinkage coefficient: n_eff = n + BekkerN1 * |slip|. This is what makes spinning wheels dig in. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Soil", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float BekkerN1 = 0.9f;

	/** Soil cohesion c [kPa]. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Soil")
	float CohesionKpa = 1.04f;

	/** Internal friction angle phi [degrees]. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Soil", meta = (ClampMin = "0.0", ClampMax = "60.0"))
	float FrictionAngleDeg = 28.f;

	/** Longitudinal shear deformation modulus K [m] (Janosi-Hanamoto). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Soil", meta = (ClampMin = "0.001"))
	float ShearDeformationK = 0.025f;

	/** Lateral shear deformation modulus K_y [m]. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Soil", meta = (ClampMin = "0.001"))
	float ShearDeformationKy = 0.030f;

	/** Soil unit weight gamma_s [kN/m^3]; feeds bulldozing resistance. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Soil", meta = (ClampMin = "0.1"))
	float UnitWeightKnPerM3 = 15.7f;

	/** SI conversion for the math library. The only place these units convert. */
	ExoneerTerramechanics::FSoilParams ToSoilParams() const;
};
