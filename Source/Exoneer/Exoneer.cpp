// Copyright Exoneer contributors. Licensed under the project license.

#include "Exoneer.h"
#include "Modules/ModuleManager.h"
#include "HAL/IConsoleManager.h"

DEFINE_LOG_CATEGORY(LogExoneer);

static TAutoConsoleVariable<bool> CVarExoneerCreative(
	TEXT("exoneer.Creative"),
	true,
	TEXT("Creative mode: welding consumes no materials and deconstruction refunds none. ")
	TEXT("Blocks welded for free refund nothing later even with this off."),
	ECVF_Default);

bool ExoneerCreative::IsEnabled()
{
	return CVarExoneerCreative.GetValueOnGameThread();
}

IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, Exoneer, "Exoneer");
