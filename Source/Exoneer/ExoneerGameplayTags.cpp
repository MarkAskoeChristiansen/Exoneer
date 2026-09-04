// Copyright Exoneer contributors.
#include "ExoneerGameplayTags.h"

namespace ExoneerTags
{
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Interaction_Use,           "Exoneer.Interaction.Use",           "Generic use/activate verb");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Interaction_OpenContainer, "Exoneer.Interaction.OpenContainer", "Opens an inventory/container UI");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Interaction_Pilot,         "Exoneer.Interaction.Pilot",         "Enter/exit a vehicle pilot seat");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Interaction_Connect,       "Exoneer.Interaction.Connect",       "Connect or disconnect an umbilical");

	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Mount_Foundation, "Exoneer.Mount.Foundation", "Snaps to terrain or foundation edge sockets");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Mount_Wall,       "Exoneer.Mount.Wall",       "Snaps to wall sockets (foundation/floor edges, wall tops)");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Mount_Floor,      "Exoneer.Mount.Floor",      "Snaps to floor sockets (wall tops, beam tops)");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Mount_Ramp,       "Exoneer.Mount.Ramp",       "Snaps to edge sockets accepting ramps");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Mount_Roof,       "Exoneer.Mount.Roof",       "Snaps to roof sockets (wall tops)");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Mount_Beam,       "Exoneer.Mount.Beam",       "Structural beams/pillars; groundable");
	UE_DEFINE_GAMEPLAY_TAG_COMMENT(Mount_Deployable, "Exoneer.Mount.Deployable", "Machines and furniture; snaps to floor/foundation surfaces");
}
