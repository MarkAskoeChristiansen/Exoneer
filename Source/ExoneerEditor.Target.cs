// Copyright Exoneer contributors. Licensed under the project license.

using UnrealBuildTool;
using System.Collections.Generic;

public class ExoneerEditorTarget : TargetRules
{
	public ExoneerEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("Exoneer");
	}
}
