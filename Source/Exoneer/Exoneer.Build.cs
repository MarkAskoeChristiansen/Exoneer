// Copyright Exoneer contributors. Licensed under the project license.

using UnrealBuildTool;

public class Exoneer : ModuleRules
{
	public Exoneer(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// The module has no Public/Private split; all includes are module-root
		// relative ("Components/X.h"). Modern build settings no longer add the
		// module directory implicitly, so do it explicitly.
		PrivateIncludePaths.Add(ModuleDirectory);

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"UMG",
			"Slate",
			"SlateCore",
			"PhysicsCore",
			"Chaos",
			"ChaosCore",
			"ChaosVehicles",
			"GameplayTags",
			"NetCore",
			"AIModule",
			"NavigationSystem"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"RenderCore",
			"RHI"
		});
	}
}
