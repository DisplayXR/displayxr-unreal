// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialExpression.h"
#include "DisplayXRMaterialExpressionStereoSelect.generated.h"

UCLASS(meta = (DisplayName = "StereoscopicSelect"))
class DISPLAYXRMATERIALS_API UDisplayXRMaterialExpressionStereoSelect : public UMaterialExpression
{
	GENERATED_BODY()

public:
	UPROPERTY(meta = (RequiredInput = "true", ToolTip = "Visible to the left view."))
	FExpressionInput Left;

	UPROPERTY(meta = (RequiredInput = "true", ToolTip = "Visible to the right view."))
	FExpressionInput Right;

#if WITH_EDITOR
	virtual void GetCaption(TArray<FString>& OutCaptions) const override;
	virtual int32 Compile(FMaterialCompiler* Compiler, int32 OutputIndex) override;
#endif
};
