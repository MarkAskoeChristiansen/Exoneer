// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"

/**
 * The 24 axis-aligned block orientations (proper rotations of a cube) and the
 * integer cell math that goes with them. Pure deterministic integer/quat
 * helpers; no UObject state.
 *
 * Orientation index layout: [Up axis 0..5] * 4 + [yaw step 0..3], where the
 * up axis order is +Z, -Z, +X, -X, +Y, -Y. Index 0 is identity.
 */
namespace ExoneerVehicleOrientation
{
	inline constexpr uint8 NumOrientations = 24;

	/** Rotation quat for an orientation index (identity for out-of-range). */
	EXONEER_API const FQuat& GetQuat(uint8 Orientation);

	/** Rotate an integer cell offset by an orientation. Exact integer result. */
	EXONEER_API FIntVector RotateOffset(const FIntVector& Offset, uint8 Orientation);

	/** Orientation that undoes the given one. */
	EXONEER_API uint8 Inverse(uint8 Orientation);

	/** Next orientation when the player taps rotate: cycles yaw, then up axis. */
	EXONEER_API uint8 CycleYaw(uint8 Orientation, int32 Steps = 1);
	EXONEER_API uint8 CycleUpAxis(uint8 Orientation, int32 Steps = 1);

	/**
	 * Cells occupied by a block of Size placed at Origin with Orientation:
	 * the rotated AABB is re-normalized so Origin remains the min corner.
	 */
	EXONEER_API void GetOccupiedCells(const FIntVector& Origin, const FIntVector& Size, uint8 Orientation, TArray<FIntVector>& OutCells);
}
