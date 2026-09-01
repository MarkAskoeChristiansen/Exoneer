// Copyright Exoneer contributors.
#include "ExoneerBootstrapLibrary.h"

FGameplayTag UExoneerBootstrapLibrary::MakeTag(FName TagName)
{
	return FGameplayTag::RequestGameplayTag(TagName, /*ErrorIfNotFound*/ false);
}

FGameplayTagContainer UExoneerBootstrapLibrary::MakeTagContainer(const TArray<FName>& TagNames)
{
	FGameplayTagContainer Container;
	for (const FName& TagName : TagNames)
	{
		const FGameplayTag Tag = MakeTag(TagName);
		if (Tag.IsValid())
		{
			Container.AddTag(Tag);
		}
	}
	return Container;
}
