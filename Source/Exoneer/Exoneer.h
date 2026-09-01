// Copyright Exoneer contributors. Licensed under the project license.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogExoneer, Log, All);

namespace ExoneerCreative
{
	/**
	 * Creative mode (console: exoneer.Creative 0/1). While on, welding
	 * consumes no materials and deconstruction refunds none - free building
	 * for playtesting. Defaults ON during the prototype phase; flip the
	 * default off when the survival economy is under test.
	 */
	EXONEER_API bool IsEnabled();
}
