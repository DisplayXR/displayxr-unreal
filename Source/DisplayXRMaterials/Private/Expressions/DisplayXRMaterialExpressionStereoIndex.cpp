// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#include "DisplayXRMaterialExpressionStereoIndex.h"
#include "MaterialCompiler.h"
#include "Materials/MaterialExpressionCustom.h"

#if WITH_EDITOR
int32 CompileDisplayXRStereoIndex(FMaterialCompiler* Compiler)
{
	UMaterialExpressionCustom* StereoExpression = NewObject<UMaterialExpressionCustom>();
	StereoExpression->OutputType = ECustomMaterialOutputType::CMOT_Float1;
	StereoExpression->Inputs = {};
	StereoExpression->Code = "return ResolvedView.StereoPassIndex;";

	TArray<int32> ExpressionInputs;
	return Compiler->CustomExpression(StereoExpression, 0, ExpressionInputs);
}

void UDisplayXRMaterialExpressionStereoIndex::GetCaption(TArray<FString>& OutCaptions) const
{
	OutCaptions.Add("Stereoscopic Index");
}

int32 UDisplayXRMaterialExpressionStereoIndex::Compile(FMaterialCompiler* Compiler, int32 OutputIndex)
{
	return CompileDisplayXRStereoIndex(Compiler);
}
#endif
