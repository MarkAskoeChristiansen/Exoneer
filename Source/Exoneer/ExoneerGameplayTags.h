// Copyright Exoneer contributors.
#pragma once

#include "CoreMinimal.h"
#include "NativeGameplayTags.h"

/**
 * Native gameplay tags used across Exoneer. Interaction tags describe the verb
 * a focused actor offers (HUD icon/prompt filtering). Mount tags drive socket
 * compatibility for base building: a socket lists the mount tags it accepts,
 * and each piece definition carries exactly one mount tag.
 */
namespace ExoneerTags
{
	// Interaction verbs
	EXONEER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Interaction_Use);
	EXONEER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Interaction_OpenContainer);
	EXONEER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Interaction_Pilot);

	// Base piece mount types
	EXONEER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Mount_Foundation);
	EXONEER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Mount_Wall);
	EXONEER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Mount_Floor);
	EXONEER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Mount_Ramp);
	EXONEER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Mount_Roof);
	EXONEER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Mount_Beam);
	EXONEER_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Mount_Deployable);
}
