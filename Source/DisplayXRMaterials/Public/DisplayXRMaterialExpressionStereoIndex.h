// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialExpression.h"
#include "DisplayXRMaterialExpressionStereoIndex.generated.h"

#if WITH_EDITOR
extern int32 CompileDisplayXRStereoIndex(FMaterialCompiler* Compiler);
#endif

UCLASS(meta = (DisplayName = "StereoscopicIndex"))
class DISPLAYXRMATERIALS_API UDisplayXRMaterialExpressionStereoIndex : public UMaterialExpression
{
	GENERATED_BODY()

public:
#if WITH_EDITOR
	virtual void GetCaption(TArray<FString>& OutCaptions) const override;
	virtual int32 Compile(FMaterialCompiler* Compiler, int32 OutputIndex) override;
#endif
};
