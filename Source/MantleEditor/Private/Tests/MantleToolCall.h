// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace MantleToolCall
{
	/**
	 * A UMantleToolset tool called by name, as Unreal's MCP server calls it: its string parameters from Args in order (the
	 * rest left empty), its JSON result. MantleEditor never links MantleAgentTools, so the tool is reached by reflection.
	 * Empty when the toolset is not loaded or has no such tool.
	 */
	inline FString CallTool(const FName Tool, const TArray<FString>& Args)
	{
		UClass* Toolset = FindObject<UClass>(nullptr, TEXT("/Script/MantleAgentTools.MantleToolset"));
		UFunction* Function = Toolset ? Toolset->FindFunctionByName(Tool) : nullptr;
		if (!Function)
		{
			return FString();
		}
		TArray<uint8> Buffer;
		Buffer.SetNumZeroed(Function->ParmsSize);
		uint8* Parms = Buffer.GetData();
		Function->InitializeStruct(Parms);
		int32 Next = 0;
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			FStrProperty* Text = CastField<FStrProperty>(*It);
			if (Text && !It->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				Text->SetPropertyValue_InContainer(Parms, Args.IsValidIndex(Next) ? Args[Next] : FString());
				++Next;
			}
		}
		Toolset->GetDefaultObject()->ProcessEvent(Function, Parms);
		FString Result;
		if (FStrProperty* Return = CastField<FStrProperty>(Function->GetReturnProperty()))
		{
			Result = Return->GetPropertyValue_InContainer(Parms);
		}
		Function->DestroyStruct(Parms);
		return Result;
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
