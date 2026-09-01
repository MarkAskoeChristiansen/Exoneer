// Copyright Exoneer contributors.
#include "Vehicles/VehicleOrientation.h"

namespace
{
	/**
	 * All tables derived from the 24 proper rotations of a cube, built once on
	 * first use. Each orientation stores its quat plus the integer images of
	 * the three axes (every component is exactly 0 or +-1), so cell math is
	 * pure integer arithmetic and fully deterministic.
	 */
	struct FOrientationTables
	{
		FQuat Quats[ExoneerVehicleOrientation::NumOrientations];
		FIntVector BasisX[ExoneerVehicleOrientation::NumOrientations];
		FIntVector BasisY[ExoneerVehicleOrientation::NumOrientations];
		FIntVector BasisZ[ExoneerVehicleOrientation::NumOrientations];
		uint8 InverseTable[ExoneerVehicleOrientation::NumOrientations];

		FOrientationTables()
		{
			// Up-axis order fixed by the header contract: +Z, -Z, +X, -X, +Y, -Y.
			const FVector UpAxes[6] =
			{
				FVector::ZAxisVector, -FVector::ZAxisVector,
				FVector::XAxisVector, -FVector::XAxisVector,
				FVector::YAxisVector, -FVector::YAxisVector
			};
			// Base quats that rotate the identity's +Z onto each up axis.
			const FQuat BaseQuats[6] =
			{
				FQuat::Identity,
				FQuat(FVector::XAxisVector, PI),
				FQuat(FVector::YAxisVector, HALF_PI),
				FQuat(FVector::YAxisVector, -HALF_PI),
				FQuat(FVector::XAxisVector, -HALF_PI),
				FQuat(FVector::XAxisVector, HALF_PI)
			};

			for (int32 Up = 0; Up < 6; ++Up)
			{
				for (int32 Yaw = 0; Yaw < 4; ++Yaw)
				{
					const int32 Index = Up * 4 + Yaw;
					// Quarter yaw turns about the NEW up axis, applied after the base tilt.
					FQuat Q = FQuat(UpAxes[Up], HALF_PI * Yaw) * BaseQuats[Up];
					Q.Normalize();
					Quats[Index] = Q;

					// Round the rotated axes to exact integer vectors.
					const auto RoundAxis = [&Q](const FVector& Axis)
					{
						const FVector R = Q.RotateVector(Axis);
						return FIntVector(FMath::RoundToInt32(R.X), FMath::RoundToInt32(R.Y), FMath::RoundToInt32(R.Z));
					};
					BasisX[Index] = RoundAxis(FVector::XAxisVector);
					BasisY[Index] = RoundAxis(FVector::YAxisVector);
					BasisZ[Index] = RoundAxis(FVector::ZAxisVector);
				}
			}

			// Index 0 must be identity by contract.
			check(BasisX[0] == FIntVector(1, 0, 0));
			check(BasisY[0] == FIntVector(0, 1, 0));
			check(BasisZ[0] == FIntVector(0, 0, 1));

			// The inverse rotation matrix is the transpose; exact integer search.
			for (int32 i = 0; i < ExoneerVehicleOrientation::NumOrientations; ++i)
			{
				InverseTable[i] = 0;
				const FIntVector TX(BasisX[i].X, BasisY[i].X, BasisZ[i].X);
				const FIntVector TY(BasisX[i].Y, BasisY[i].Y, BasisZ[i].Y);
				const FIntVector TZ(BasisX[i].Z, BasisY[i].Z, BasisZ[i].Z);
				for (int32 j = 0; j < ExoneerVehicleOrientation::NumOrientations; ++j)
				{
					if (BasisX[j] == TX && BasisY[j] == TY && BasisZ[j] == TZ)
					{
						InverseTable[i] = static_cast<uint8>(j);
						break;
					}
				}
			}
		}
	};

	const FOrientationTables& GetTables()
	{
		static const FOrientationTables Tables;
		return Tables;
	}
}

namespace ExoneerVehicleOrientation
{

const FQuat& GetQuat(uint8 Orientation)
{
	if (Orientation >= NumOrientations)
	{
		return FQuat::Identity;
	}
	return GetTables().Quats[Orientation];
}

FIntVector RotateOffset(const FIntVector& Offset, uint8 Orientation)
{
	if (Orientation >= NumOrientations)
	{
		return Offset;
	}
	const FOrientationTables& T = GetTables();
	return T.BasisX[Orientation] * Offset.X
	     + T.BasisY[Orientation] * Offset.Y
	     + T.BasisZ[Orientation] * Offset.Z;
}

uint8 Inverse(uint8 Orientation)
{
	if (Orientation >= NumOrientations)
	{
		return 0;
	}
	return GetTables().InverseTable[Orientation];
}

uint8 FindOrientationMappingAxis(const FVector& LocalAxis, const FVector& TargetWorld, float Tolerance)
{
	for (uint8 Candidate = 0; Candidate < NumOrientations; ++Candidate)
	{
		if (GetQuat(Candidate).RotateVector(LocalAxis).Equals(TargetWorld, Tolerance))
		{
			return Candidate;
		}
	}
	return 0;
}

uint8 CycleYaw(uint8 Orientation, int32 Steps)
{
	if (Orientation >= NumOrientations)
	{
		Orientation = 0;
	}
	const int32 Up = Orientation / 4;
	int32 Yaw = ((Orientation % 4) + Steps) % 4;
	if (Yaw < 0)
	{
		Yaw += 4;
	}
	return static_cast<uint8>(Up * 4 + Yaw);
}

uint8 CycleUpAxis(uint8 Orientation, int32 Steps)
{
	if (Orientation >= NumOrientations)
	{
		Orientation = 0;
	}
	int32 Up = ((Orientation / 4) + Steps) % 6;
	if (Up < 0)
	{
		Up += 6;
	}
	return static_cast<uint8>(Up * 4 + (Orientation % 4));
}

void GetOccupiedCells(const FIntVector& Origin, const FIntVector& Size, uint8 Orientation, TArray<FIntVector>& OutCells)
{
	OutCells.Reset();

	const FIntVector SafeSize(FMath::Max(1, Size.X), FMath::Max(1, Size.Y), FMath::Max(1, Size.Z));
	// Rotate the far extent corner; the rotated AABB spans |R| cells past the
	// origin on each axis once re-normalized so Origin stays the min corner.
	const FIntVector R = RotateOffset(SafeSize - FIntVector(1, 1, 1), Orientation);
	const FIntVector Extent(FMath::Abs(R.X), FMath::Abs(R.Y), FMath::Abs(R.Z));

	OutCells.Reserve((Extent.X + 1) * (Extent.Y + 1) * (Extent.Z + 1));
	for (int32 X = 0; X <= Extent.X; ++X)
	{
		for (int32 Y = 0; Y <= Extent.Y; ++Y)
		{
			for (int32 Z = 0; Z <= Extent.Z; ++Z)
			{
				OutCells.Add(Origin + FIntVector(X, Y, Z));
			}
		}
	}
}

} // namespace ExoneerVehicleOrientation
