// Copyright Exoneer contributors.
#include "Vehicles/PilotInput.h"

namespace
{
	// [-1, 1] <-> int8 fixed point. 8 bits per axis is ample for intents.
	void SerializeSignedAxis(FArchive& Ar, float& Value)
	{
		if (Ar.IsSaving())
		{
			int8 Quantized = (int8)FMath::RoundToInt(FMath::Clamp(Value, -1.f, 1.f) * 127.f);
			Ar << Quantized;
		}
		else
		{
			int8 Quantized = 0;
			Ar << Quantized;
			Value = (float)Quantized / 127.f;
		}
	}
}

bool FPilotInput::NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
{
	float MoveX = Move.X, MoveY = Move.Y, MoveZ = Move.Z;
	float RotX = Rotate.X, RotY = Rotate.Y, RotZ = Rotate.Z;
	SerializeSignedAxis(Ar, MoveX);
	SerializeSignedAxis(Ar, MoveY);
	SerializeSignedAxis(Ar, MoveZ);
	SerializeSignedAxis(Ar, RotX);
	SerializeSignedAxis(Ar, RotY);
	SerializeSignedAxis(Ar, RotZ);
	SerializeSignedAxis(Ar, Throttle);
	SerializeSignedAxis(Ar, Steer);
	Move = FVector(MoveX, MoveY, MoveZ);
	Rotate = FVector(RotX, RotY, RotZ);

	if (Ar.IsSaving())
	{
		uint8 QuantizedBrake = (uint8)FMath::RoundToInt(FMath::Clamp(Brake, 0.f, 1.f) * 255.f);
		Ar << QuantizedBrake;
		uint8 Packed = (HeldFlags & 0x3F) | ((ModeToggleCount & 0x3) << 6);
		Ar << Packed;
	}
	else
	{
		uint8 QuantizedBrake = 0;
		Ar << QuantizedBrake;
		Brake = (float)QuantizedBrake / 255.f;
		uint8 Packed = 0;
		Ar << Packed;
		HeldFlags = Packed & 0x3F;
		ModeToggleCount = (Packed >> 6) & 0x3;
	}

	bOutSuccess = true;
	return true;
}
