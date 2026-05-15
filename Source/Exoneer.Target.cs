// Copyright Exoneer contributors. Licensed under the project license.

using UnrealBuildTool;
using System.Collections.Generic;

public class ExoneerTarget : TargetRules
{
	public ExoneerTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("Exoneer");
	}
}
