// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#include "DisplayXRMaterialExpressionStereoSelect.h"
#include "DisplayXRMaterialExpressionStereoIndex.h"
#include "MaterialCompiler.h"

#if WITH_EDITOR
void UDisplayXRMaterialExpressionStereoSelect::GetCaption(TArray<FString>& OutCaptions) const
{
	OutCaptions.Add("Stereoscopic Select");
}

int32 UDisplayXRMaterialExpressionStereoSelect::Compile(FMaterialCompiler* Compiler, int32 OutputIndex)
{
	if (!Left.GetTracedInput().Expression)
	{
		return Compiler->Errorf(TEXT("Missing Left"));
	}
	if (!Right.GetTracedInput().Expression)
	{
		return Compiler->Errorf(TEXT("Missing Right"));
	}

	int32 InLeft = Left.Compile(Compiler);
	int32 InRight = Right.Compile(Compiler);
	int32 Alpha = CompileDisplayXRStereoIndex(Compiler);
	return Compiler->Lerp(InLeft, InRight, Alpha);
}
#endif
