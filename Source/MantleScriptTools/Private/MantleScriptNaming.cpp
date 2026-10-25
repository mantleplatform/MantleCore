// Copyright Joshua Eger. All Rights Reserved.
#include "MantleScriptNaming.h"

FString FMantleNaming::ToCamel(const FString& Pascal)
{
	if (Pascal.IsEmpty())
	{
		return Pascal;
	}
	// Leading acronym run: "UIText" -> "uiText", "ID" -> "id".
	int32 Upper = 0;
	while (Upper < Pascal.Len() && FChar::IsUpper(Pascal[Upper]))
	{
		++Upper;
	}
	FString Out = Pascal;
	if (Upper <= 1)
	{
		Out[0] = FChar::ToLower(Out[0]);
	}
	else
	{
		const int32 LowerCount = Upper == Pascal.Len() ? Upper : Upper - 1;
		for (int32 i = 0; i < LowerCount; ++i)
		{
			Out[i] = FChar::ToLower(Out[i]);
		}
	}
	return Out;
}

FString FMantleNaming::ToSnake(const FString& Pascal)
{
	FString Out;
	for (int32 i = 0; i < Pascal.Len(); ++i)
	{
		const TCHAR C = Pascal[i];
		if (FChar::IsUpper(C))
		{
			const bool bPrevLower = i > 0 && !FChar::IsUpper(Pascal[i - 1]);
			const bool bNextLower = i + 1 < Pascal.Len() && FChar::IsLower(Pascal[i + 1]);
			if (i > 0 && (bPrevLower || (bNextLower && FChar::IsUpper(Pascal[i - 1]))))
			{
				Out.AppendChar(TEXT('_'));
			}
			Out.AppendChar(FChar::ToLower(C));
		}
		else
		{
			Out.AppendChar(C);
		}
	}
	return Out;
}
