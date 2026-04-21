// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include "CoreMinimal.h"
#include "Materials/MaterialExpression.h"
#include "DisplayXRMaterialExpressionSideBySideCoords.generated.h"

UCLASS(meta = (DisplayName = "SideBySideCoordinate"))
class DISPLAYXRMATERIALS_API UDisplayXRMaterialExpressionSideBySideCoord : public UMaterialExpression
{
	GENERATED_BODY()

public:
	UPROPERTY(meta = (RequiredInput = "true", ToolTip = "Texture coordinate"))
	FExpressionInput Coord;

#if WITH_EDITOR
	virtual void GetCaption(TArray<FString>& OutCaptions) const override;
	virtual int32 Compile(FMaterialCompiler* Compiler, int32 OutputIndex) override;
#endif
};
