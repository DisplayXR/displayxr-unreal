// Copyright 2025-2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0

#include "DisplayXRMaterialExpressionSideBySideCoords.h"
#include "DisplayXRMaterialExpressionStereoIndex.h"
#include "MaterialCompiler.h"

#if WITH_EDITOR
void UDisplayXRMaterialExpressionSideBySideCoord::GetCaption(TArray<FString>& OutCaptions) const
{
	OutCaptions.Add("SideBySide Coordinate");
}

int32 UDisplayXRMaterialExpressionSideBySideCoord::Compile(FMaterialCompiler* Compiler, int32 OutputIndex)
{
	if (!Coord.GetTracedInput().Expression)
	{
		return Compiler->Errorf(TEXT("Missing Coord"));
	}

	int32 InCoord = Coord.Compile(Compiler);
	int32 CoordVec2 = Compiler->ValidCast(InCoord, MCT_Float2);
	int32 ConstDivide = Compiler->Constant2(2, 1);
	int32 ViewCoord = Compiler->Div(CoordVec2, ConstDivide);
	int32 ConstMultiply = Compiler->Constant2(0.5f, 0);
	int32 StereoIndex = CompileDisplayXRStereoIndex(Compiler);
	int32 StereoOffset = Compiler->Mul(ConstMultiply, StereoIndex);
	return Compiler->Add(ViewCoord, StereoOffset);
}
#endif
