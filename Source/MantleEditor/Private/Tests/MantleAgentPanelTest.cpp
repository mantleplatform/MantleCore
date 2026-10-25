// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarConverter.h"
#include "MantleCosmeticPanel.h"
#include "MantleEditorActions.h"
#include "MantleVehicleScratchBody.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/MetaData.h"
#include "UObject/UnrealType.h"

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

#if WITH_DEV_AUTOMATION_TESTS

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleAgentPanelTestPrivate
{
	/**
	 * A UMantleToolset tool called by name, as Unreal's MCP server calls it: its string parameters from Args in order (the
	 * rest left empty), its JSON result. MantleEditor never links MantleAgentTools, so the tool is reached by reflection.
	 * Empty when the toolset is not loaded or has no such tool.
	 */
	FString CallTool(const FName Tool, const TArray<FString>& Args)
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

	FString SeverityName(const EMantleCosmeticSeverity Severity)
	{
		switch (Severity)
		{
		case EMantleCosmeticSeverity::Error: return TEXT("error");
		case EMantleCosmeticSeverity::Warning: return TEXT("warning");
		default: return TEXT("info");
		}
	}

	/** "severity: message | hint", one per issue, as both sides are compared. */
	TArray<FString> Lines(const TArray<FMantleCosmeticIssue>& Issues)
	{
		TArray<FString> Out;
		for (const FMantleCosmeticIssue& Issue : Issues)
		{
			Out.Add(FString::Printf(TEXT("%s: %s | %s"), *SeverityName(Issue.Severity), *Issue.Message, *Issue.Hint));
		}
		return Out;
	}

	TArray<FString> Lines(const TSharedPtr<FJsonObject>& Check)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* Issues = nullptr;
		if (Check.IsValid() && Check->TryGetArrayField(TEXT("issues"), Issues))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Issues)
			{
				const TSharedPtr<FJsonObject> Issue = Value->AsObject();
				Out.Add(FString::Printf(TEXT("%s: %s | %s"), *Issue->GetStringField(TEXT("severity")), *Issue->GetStringField(TEXT("message")),
					*Issue->GetStringField(TEXT("hint"))));
			}
		}
		return Out;
	}
}

// Chunk 8, agent = panel: the toolset's CheckCosmetic and the Cosmetics panel's Check give the same result and the same
// messages on one synthetic mesh, as clothing and as an avatar; and the preparation both share.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleAgentPanelTest,
	"Mantle.Editor.CosmeticPanel.AgentSameAsPanel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleAgentPanelTest::RunTest(const FString& Parameters)
{
	using namespace MantleAgentPanelTestPrivate;

	// The publication mode both send: an avatar's by route, nothing for another class.
	TestEqual(TEXT("avatar on Convert"), FMantleEditorActions::AvatarPublicationMode(EMantleCosmeticClass::Avatar, EMantleAvatarRoute::Convert), FString(TEXT("converted")));
	TestEqual(TEXT("avatar As is"), FMantleEditorActions::AvatarPublicationMode(EMantleCosmeticClass::Avatar, EMantleAvatarRoute::AsIs), FString(TEXT("as_is")));
	TestEqual(TEXT("clothing has none"), FMantleEditorActions::AvatarPublicationMode(EMantleCosmeticClass::Garment, EMantleAvatarRoute::AsIs), FString());

	FString Error;
	USkeletalMesh* Mesh = MantleVehicleScratchBody::Make(TEXT("/Temp/MantleAgentPanelTest/SK_AgentPanelPiece"), TEXT("SK_AgentPanelPiece"), Error);
	if (!TestNotNull(FString::Printf(TEXT("the synthetic mesh (%s)"), *Error), Mesh))
	{
		return false;
	}
	ON_SCOPE_EXIT { MantleVehicleScratchBody::Discard(Mesh); };

	// The route a body was converted on: a mesh nobody converted goes up as Convert (the panel's pre-selected choice).
	TestTrue(TEXT("unstamped mesh: Convert"), FMantleEditorActions::CosmeticRouteOf(Mesh) == EMantleAvatarRoute::Convert);
	TestNull(TEXT("no converted face for a mesh nobody converted"), FMantleEditorActions::FindConvertedFace(Mesh));

	// The same piece through the panel and the agent, as clothing and as an avatar.
	const TCHAR* const Slots[] = { TEXT("top"), TEXT("avatar") };
	const EMantleCosmeticClass Classes[] = { EMantleCosmeticClass::Garment, EMantleCosmeticClass::Avatar };
	for (int32 Case = 0; Case < UE_ARRAY_COUNT(Slots); ++Case)
	{
		TArray<FMantleCosmeticIssue> PanelIssues;
		const bool bPanelPassed = SMantleCosmeticPanel::DevCheck(Classes[Case], Mesh, Slots[Case], PanelIssues);
		const FString Json = CallTool(TEXT("CheckCosmetic"), { Mesh->GetPathName(), Slots[Case], FString(), FString(), FString() });
		if (!TestFalse(TEXT("the agent toolset is loaded (MantleAgentTools, with the Toolset Registry on)"), Json.IsEmpty()))
		{
			return false;
		}
		TSharedPtr<FJsonObject> Check;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Check);
		if (!TestTrue(FString::Printf(TEXT("%s: the agent's check ran (%s)"), Slots[Case], *Json), Check.IsValid() && Check->GetBoolField(TEXT("ok"))))
		{
			continue;
		}
		TestEqual(FString::Printf(TEXT("%s: same result"), Slots[Case]), Check->GetBoolField(TEXT("canUpload")), bPanelPassed);
		const TArray<FString> Panel = Lines(PanelIssues);
		const TArray<FString> Agent = Lines(Check);
		TestEqual(FString::Printf(TEXT("%s: same number of messages"), Slots[Case]), Agent.Num(), Panel.Num());
		for (int32 Index = 0; Index < FMath::Max(Panel.Num(), Agent.Num()); ++Index)
		{
			TestEqual(FString::Printf(TEXT("%s: message %d"), Slots[Case], Index), Agent.IsValidIndex(Index) ? Agent[Index] : FString(),
				Panel.IsValidIndex(Index) ? Panel[Index] : FString());
		}
		TestTrue(FString::Printf(TEXT("%s: the case says something to compare"), Slots[Case]), Panel.Num() > 0);
	}

	// ConvertCosmetic through the tool, list only: the panel's "what I see" line for the piece and its Convert button state.
	{
		const FString Json = CallTool(TEXT("ConvertCosmetic"), { Mesh->GetPathName(), TEXT("avatar"), FString(), FString(), TEXT("convert") });
		TSharedPtr<FJsonObject> Convert;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Convert);
		if (TestTrue(FString::Printf(TEXT("ConvertCosmetic answered (%s)"), *Json.Left(400)), Convert.IsValid()) && Convert->GetBoolField(TEXT("ok")))
		{
			TestTrue(FString::Printf(TEXT("the what-I-see line names the piece (%s)"), *Convert->GetStringField(TEXT("sees"))),
				Convert->GetStringField(TEXT("sees")).StartsWith(Mesh->GetName() + TEXT(": Skeletal mesh")));
			TestTrue(TEXT("the Convert button state is reported"), Convert->HasField(TEXT("canConvert")));
			TestEqual(TEXT("route"), Convert->GetStringField(TEXT("route")), FString(TEXT("convert")));
		}
	}

	// A body stamped As-is goes up as As is.
	Mesh->GetPackage()->GetMetaData().SetValue(Mesh, FMantleAvatarConverter::RouteKey, LexToString(EMantleAvatarRoute::AsIs));
	TestTrue(TEXT("stamped As-is: As is"), FMantleEditorActions::CosmeticRouteOf(Mesh) == EMantleAvatarRoute::AsIs);

	// ConvertCosmetic's route: a word the panel doesn't offer is refused before anything is looked at.
	const FString Route = FMantleAvatarConverter::RunForAgent(Mesh->GetPathName(), TEXT("avatar"), false, FString(), FString(), TEXT("sideways"));
	TestTrue(FString::Printf(TEXT("unknown route refused (%s)"), *Route), Route.Contains(TEXT("\"unknown_route\"")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
