// Copyright (c) GaussianSplatSequence contributors. MIT License.

using UnrealBuildTool;

public class GaussianSplatSequence : ModuleRules
{
	public GaussianSplatSequence(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate",
			"SlateCore",
			"InputCore",
			"UnrealEd",
			"LevelEditor",
			"ToolMenus",
			"PropertyEditor",
			"CinematicCamera",
			"ImageCore",
			"ImageWrapper",
			"MovieScene",
			"LevelSequence",
			"LevelSequenceEditor",
		});
	}
}
