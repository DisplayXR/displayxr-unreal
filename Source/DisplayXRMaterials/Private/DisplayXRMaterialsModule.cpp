// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#include "Modules/ModuleManager.h"

class FDisplayXRMaterialsModule : public IModuleInterface
{
public:
	virtual void StartupModule() override {}
	virtual void ShutdownModule() override {}
};

IMPLEMENT_MODULE(FDisplayXRMaterialsModule, DisplayXRMaterials)
