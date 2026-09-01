// Copyright Exoneer contributors.
#include "Physics/ExoneerSoilPhysicalMaterial.h"

ExoneerTerramechanics::FSoilParams UExoneerSoilPhysicalMaterial::ToSoilParams() const
{
	ExoneerTerramechanics::FSoilParams Soil;
	Soil.Kc = BekkerKc * 1000.f;                                   // kN/m^(n+1) -> N/m^(n+1)
	Soil.Kphi = BekkerKphi * 1000.f;                               // kN/m^(n+2) -> N/m^(n+2)
	Soil.N0 = BekkerN;
	Soil.N1 = BekkerN1;
	Soil.Cohesion = CohesionKpa * 1000.f;                          // kPa -> Pa
	Soil.FrictionAngleRad = FMath::DegreesToRadians(FrictionAngleDeg);
	Soil.ShearK = ShearDeformationK;
	Soil.ShearKy = ShearDeformationKy;
	Soil.UnitWeight = UnitWeightKnPerM3 * 1000.f;                  // kN/m^3 -> N/m^3
	return Soil;
}
