// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarContract.h"
#include "MantleBodyCorrespondence.h"
#include "MantleEditorActions.h"
#include "MantleFaceLayout.h"
#include "MantleFitBodies.h"

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

// Garment variants are fitted to Epic's standard body types. The pure parts: the type names, the list read from limits.json,
// the variant ids and the sex choice. Nothing here makes a MetaHuman character.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleFitBodiesTest,
	"Mantle.Editor.FitBodies.Pure",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleFitBodiesTest::RunTest(const FString& Parameters)
{
	// Type names map to Epic's enum, and nothing else does.
	EMetaHumanBodyType Type = EMetaHumanBodyType::Count;
	TestTrue(TEXT("f_med_nrw parses"), FMantleFitBodies::ParseBodyType(TEXT("f_med_nrw"), Type));
	TestTrue(TEXT("f_med_nrw is Epic's f_med_nrw"), Type == EMetaHumanBodyType::f_med_nrw);
	TestTrue(TEXT("m_med_ovw parses"), FMantleFitBodies::ParseBodyType(TEXT("m_med_ovw"), Type));
	TestTrue(TEXT("m_med_ovw is Epic's m_med_ovw"), Type == EMetaHumanBodyType::m_med_ovw);
	TestTrue(TEXT("f_tal_unw parses"), FMantleFitBodies::ParseBodyType(TEXT("f_tal_unw"), Type));
	TestTrue(TEXT("f_tal_unw is Epic's f_tal_unw"), Type == EMetaHumanBodyType::f_tal_unw);
	TestTrue(TEXT("m_srt_unw parses"), FMantleFitBodies::ParseBodyType(TEXT("m_srt_unw"), Type));
	TestTrue(TEXT("m_srt_unw is Epic's m_srt_unw"), Type == EMetaHumanBodyType::m_srt_unw);
	for (const TCHAR* Bad : { TEXT(""), TEXT("aera"), TEXT("bo"), TEXT("med_nrw"), TEXT("x_med_nrw"), TEXT("f_med"), TEXT("f_big_nrw"), TEXT("f_med_xxx"), TEXT("F_MED_NRW") })
	{
		TestFalse(FString::Printf(TEXT("\"%s\" is not a standard body type"), Bad), FMantleFitBodies::ParseBodyType(Bad, Type));
	}

	// The list: valid, distinct, capped, in order.
	const TArray<FString> Cleaned = FMantleFitBodies::SanitizeBodyTypes({ TEXT("med_unw"), TEXT("nope"), TEXT("med_nrw"), TEXT("med_unw"), TEXT("f_med_ovw"), TEXT("tal_ovw") });
	TestEqual(TEXT("invalid and repeated entries are dropped"), FString::Join(Cleaned, TEXT(",")), FString(TEXT("med_unw,med_nrw,tal_ovw")));
	const TArray<FString> Capped = FMantleFitBodies::SanitizeBodyTypes({ TEXT("med_unw"), TEXT("med_nrw"), TEXT("med_ovw"), TEXT("srt_nrw") }, 2);
	TestEqual(TEXT("the list is capped"), Capped.Num(), 2);

	// The shipped garment-fit.json lists the three medium-height builds, within the backend's 1 to 8 variants.
	FString ContractError;
	if (const FMantleAvatarContract* Contract = FMantleAvatarContract::Get(&ContractError))
	{
		TestEqual(TEXT("garment-fit.json bodyTypes"), FString::Join(Contract->GarmentFitBodyTypes, TEXT(",")), FString(TEXT("med_unw,med_nrw,med_ovw")));
		TestTrue(TEXT("one to eight variants"), Contract->GarmentFitBodyTypes.Num() >= 1 && Contract->GarmentFitBodyTypes.Num() <= 8);
		TestTrue(TEXT("every listed type is a standard body"), FMantleFitBodies::SanitizeBodyTypes(Contract->GarmentFitBodyTypes).Num() == Contract->GarmentFitBodyTypes.Num());
		TestTrue(TEXT("the sex margin is not negative"), Contract->GarmentFitSexMarginCm >= 0.0f);
	}
	else
	{
		AddError(FString::Printf(TEXT("the avatar contract did not load: %s"), *ContractError));
	}

	// Variant ids are the type names, valid for the backend (1 to 32 lowercase letters, digits, _ or -, starting with a letter).
	const TArray<FString> Suffixes = { TEXT("med_unw"), TEXT("med_nrw"), TEXT("med_ovw") };
	TestEqual(TEXT("female ids"), FString::Join(FMantleFitBodies::VariantIds(EMantleFitSex::Female, Suffixes), TEXT(",")), FString(TEXT("f_med_unw,f_med_nrw,f_med_ovw")));
	TestEqual(TEXT("male ids"), FString::Join(FMantleFitBodies::VariantIds(EMantleFitSex::Male, Suffixes), TEXT(",")), FString(TEXT("m_med_unw,m_med_nrw,m_med_ovw")));
	TestEqual(TEXT("Auto is read as female"), FMantleFitBodies::VariantIds(EMantleFitSex::Auto, Suffixes)[0], FString(TEXT("f_med_unw")));
	for (const FString& Id : FMantleFitBodies::VariantIds(EMantleFitSex::Male, Suffixes))
	{
		EMetaHumanBodyType IdType = EMetaHumanBodyType::Count;
		TestTrue(FString::Printf(TEXT("%s is a standard body type"), *Id), FMantleFitBodies::ParseBodyType(Id, IdType));
		TestTrue(FString::Printf(TEXT("%s is a valid variant id"), *Id), Id.Len() >= 1 && Id.Len() <= 32 && FChar::IsLower(Id[0]) && Id == Id.ToLower());
	}

	// The sex: male only when clearly closer to the male body; unclear or unreadable is female.
	FString Why;
	TestTrue(TEXT("close to the female body is female"), FMantleFitBodies::ChooseSex({ 0.9f, 2.0f }, { 4.0f, 5.0f }, 0.25f, Why) == EMantleFitSex::Female);
	TestTrue(TEXT("close to the male body is male"), FMantleFitBodies::ChooseSex({ 4.0f, 5.0f }, { 1.1f, 2.0f }, 0.25f, Why) == EMantleFitSex::Male);
	TestTrue(TEXT("the nearest size of each decides"), FMantleFitBodies::ChooseSex({ 5.0f, 0.8f, 6.0f }, { 3.0f, 3.5f, 4.0f }, 0.25f, Why) == EMantleFitSex::Female);
	TestTrue(TEXT("too close to call is female"), FMantleFitBodies::ChooseSex({ 1.0f }, { 0.9f }, 0.25f, Why) == EMantleFitSex::Female);
	TestTrue(TEXT("unreadable sizes are skipped"), FMantleFitBodies::ChooseSex({ -1.0f, 4.0f }, { -1.0f, 1.0f }, 0.25f, Why) == EMantleFitSex::Male);
	TestTrue(TEXT("nothing readable is female"), FMantleFitBodies::ChooseSex({ -1.0f }, { -1.0f }, 0.25f, Why) == EMantleFitSex::Female);
	TestTrue(TEXT("no sizes is female"), FMantleFitBodies::ChooseSex({}, {}, 0.25f, Why) == EMantleFitSex::Female);
	TestTrue(TEXT("one side unreadable is female"), FMantleFitBodies::ChooseSex({ -1.0f }, { 1.0f }, 0.25f, Why) == EMantleFitSex::Female);
	TestFalse(TEXT("the choice comes with a reason"), Why.IsEmpty());

	// The agent tools' FitSex: "", auto, female or male, in any case; anything else is refused.
	EMantleFitSex Sex = EMantleFitSex::Male;
	TestTrue(TEXT("\"\" is accepted"), FMantleEditorActions::ParseFitSex(TEXT(""), Sex) && Sex == EMantleFitSex::Auto);
	TestTrue(TEXT("auto is accepted"), FMantleEditorActions::ParseFitSex(TEXT(" Auto "), Sex) && Sex == EMantleFitSex::Auto);
	TestTrue(TEXT("female is accepted"), FMantleEditorActions::ParseFitSex(TEXT("female"), Sex) && Sex == EMantleFitSex::Female);
	TestTrue(TEXT("male is accepted"), FMantleEditorActions::ParseFitSex(TEXT("MALE"), Sex) && Sex == EMantleFitSex::Male);
	TestFalse(TEXT("f is refused"), FMantleEditorActions::ParseFitSex(TEXT("f"), Sex));
	TestFalse(TEXT("women is refused"), FMantleEditorActions::ParseFitSex(TEXT("women"), Sex));
	return true;
}

// Creator's in-memory body has the reference UV layout but Epic's build flipped a few quad diagonals: a tiny layout of two quads.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleFlippedDiagonalsTest,
	"Mantle.Editor.FitBodies.FlippedDiagonals",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleFlippedDiagonalsTest::RunTest(const FString& Parameters)
{
	// Reference: a 3 x 2 grid of vertices (id = y * 3 + x), two quads, each cut one way.
	FMantleBodyCorrespondence Layout;
	for (int32 Y = 0; Y < 2; ++Y)
	{
		for (int32 X = 0; X < 3; ++X) { Layout.QuantizedUVs.Add(FIntPoint(X * 2048 + 100, Y * 2048 + 100)); }
	}
	Layout.Indices = { 0, 1, 4,  0, 4, 3,  1, 2, 5,  1, 5, 4 };

	// The input numbers the vertices differently: input vertex Permutation[r] is reference vertex r.
	const TArray<int32> Permutation = { 3, 5, 0, 1, 4, 2 };
	const auto MakeInput = [&Layout, &Permutation](const TArray<uint32>& ReferenceTriangles, TArray<FVector2f>& OutUVs, TArray<uint32>& OutIndices)
	{
		OutUVs.SetNum(Layout.QuantizedUVs.Num());
		for (int32 Reference = 0; Reference < Layout.QuantizedUVs.Num(); ++Reference)
		{
			OutUVs[Permutation[Reference]] = FVector2f(Layout.QuantizedUVs[Reference].X / 2048.0f, Layout.QuantizedUVs[Reference].Y / 2048.0f);
		}
		OutIndices.Reset();
		for (const uint32 Reference : ReferenceTriangles) { OutIndices.Add(Permutation[Reference]); }
	};

	TArray<FVector2f> UVs;
	TArray<uint32> Indices;
	TArray<int32> InputOfReference;
	FString Note, Error;

	MakeInput(Layout.Indices, UVs, Indices);
	TestTrue(TEXT("the same triangles, renumbered, match"), Layout.MatchFlippedDiagonals(UVs, Indices, InputOfReference, Note, Error));
	TestEqual(TEXT("each reference vertex finds its input vertex"), FString::JoinBy(InputOfReference, TEXT(","), [](const int32 Index) { return FString::FromInt(Index); }), FString(TEXT("3,5,0,1,4,2")));

	MakeInput({ 0, 1, 3,  1, 4, 3,  1, 2, 5,  1, 5, 4 }, UVs, Indices);
	TestTrue(TEXT("one quad cut the other way matches"), Layout.MatchFlippedDiagonals(UVs, Indices, InputOfReference, Note, Error));
	TestTrue(TEXT("the note says a diagonal was flipped"), Note.Contains(TEXT("1 diagonal")));

	MakeInput({ 0, 1, 3,  1, 4, 3,  1, 2, 4,  2, 5, 4 }, UVs, Indices);
	TestTrue(TEXT("both quads cut the other way match"), Layout.MatchFlippedDiagonals(UVs, Indices, InputOfReference, Note, Error));
	TestTrue(TEXT("the note counts both"), Note.Contains(TEXT("2 diagonal")));

	MakeInput({ 0, 1, 4,  0, 4, 3,  1, 2, 5,  1, 5, 3 }, UVs, Indices);
	TestFalse(TEXT("a changed triangle that is not a flipped diagonal is refused"), Layout.MatchFlippedDiagonals(UVs, Indices, InputOfReference, Note, Error));

	MakeInput(Layout.Indices, UVs, Indices);
	UVs[0].X += 0.5f;
	TestFalse(TEXT("a different UV layout is refused"), Layout.MatchFlippedDiagonals(UVs, Indices, InputOfReference, Note, Error));
	return true;
}

// A face made in memory is put into the built face layout: two sections, one of them cut the other way, renumbered, with per-vertex data carried along.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleFaceLayoutTest,
	"Mantle.Editor.FitBodies.FaceLayout",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleFaceLayoutTest::RunTest(const FString& Parameters)
{
	// Layout: section "skin" is a 3 x 2 grid (vertices 0..5, two quads), section "teeth" a single triangle (vertices 6..8).
	FMantleFaceLayout Layout;
	Layout.Source = TEXT("test");
	for (int32 Y = 0; Y < 2; ++Y) { for (int32 X = 0; X < 3; ++X) { Layout.QuantizedUVs.Add(FIntPoint(X * 2048 + 100, Y * 2048 + 100)); } }
	Layout.QuantizedUVs.Add(FIntPoint(9000, 9000));
	Layout.QuantizedUVs.Add(FIntPoint(9100, 9000));
	Layout.QuantizedUVs.Add(FIntPoint(9000, 9100));
	Layout.Indices = { 0, 1, 4,  0, 4, 3,  1, 2, 5,  1, 5, 4,  6, 7, 8 };
	Layout.Sections = { { TEXT("skin"), 0, 6, 0, 4 }, { TEXT("teeth"), 6, 3, 12, 1 } };

	FMantleFaceLayout Read;
	FString Error;
	TestTrue(TEXT("a written layout reads back"), FMantleFaceLayout::Parse(Layout.ToJson(), Read, Error));
	TestEqual(TEXT("sections survive"), Read.Sections.Num(), 2);
	TestEqual(TEXT("indices survive"), Read.Indices.Num(), 15);
	TestFalse(TEXT("text that is not a layout is refused"), FMantleFaceLayout::Parse(TEXT("{}"), Read, Error));

	// The face: the same vertices renumbered (reference vertex r is input vertex InputOfReference[r]), the skin's first quad cut the other way.
	FMantleGlbMesh Mesh;
	Mesh.NumInfluences = 1;
	Mesh.MaterialNames = { TEXT("skin_mat"), TEXT("teeth_mat") };
	const int32 InputOfReference[9] = { 3, 0, 5, 1, 2, 4, 8, 6, 7 };
	Mesh.Positions.SetNum(9);
	Mesh.UVs.SetNum(9);
	Mesh.InfluenceJoints.SetNum(9);
	Mesh.InfluenceWeights.SetNum(9);
	for (int32 Reference = 0; Reference < 9; ++Reference)
	{
		const int32 Input = InputOfReference[Reference];
		Mesh.Positions[Input] = FVector3f(static_cast<float>(Reference), 0.0f, 0.0f);
		Mesh.UVs[Input] = FVector2f(Layout.QuantizedUVs[Reference].X / 2048.0f, Layout.QuantizedUVs[Reference].Y / 2048.0f);
		Mesh.InfluenceJoints[Input] = static_cast<uint16>(Reference);
		Mesh.InfluenceWeights[Input] = 1.0f;
	}
	FMantleGlbPrimitive& Skin = Mesh.Primitives.AddDefaulted_GetRef();
	Skin.MaterialIndex = 0; Skin.FirstVertex = 0; Skin.NumVertices = 6;
	// Reference triangles (0,1,4)(0,4,3)(1,2,5)(1,5,4), the first quad cut the other way: (0,1,3)(1,4,3).
	for (const int32 Reference : { 0, 1, 3,  1, 4, 3,  1, 2, 5,  1, 5, 4 }) { Skin.Indices.Add(InputOfReference[Reference]); }
	FMantleGlbPrimitive& Teeth = Mesh.Primitives.AddDefaulted_GetRef();
	Teeth.MaterialIndex = 1; Teeth.FirstVertex = 6; Teeth.NumVertices = 3;
	for (const int32 Reference : { 6, 7, 8 }) { Teeth.Indices.Add(InputOfReference[Reference] - 6); }

	FMantleGlbMesh Original = Mesh;
	FString Note;
	TestFalse(TEXT("a face with another vertex count is refused"), [&Layout, &Original]()
	{
		FMantleGlbMesh Short = Original;
		Short.Positions.Pop();
		FString Note2, Error2;
		return Layout.Reorder(Short, Note2, Error2);
	}());
	TestTrue(TEXT("the face is put in the layout"), Layout.Reorder(Mesh, Note, Error));
	if (Mesh.Positions.Num() == 9)
	{
		bool bCarried = true;
		for (int32 Reference = 0; Reference < 9; ++Reference)
		{
			bCarried &= FMath::IsNearlyEqual(Mesh.Positions[Reference].X, static_cast<float>(Reference)) && Mesh.InfluenceJoints[Reference] == Reference;
		}
		TestTrue(TEXT("positions and skin follow the vertices"), bCarried);
		TestEqual(TEXT("the sections are the layout's"), Mesh.Primitives.Num(), 2);
		TestTrue(TEXT("the skin keeps its material"), Mesh.Primitives.Num() == 2 && Mesh.Primitives[0].MaterialIndex == 0 && Mesh.Primitives[1].MaterialIndex == 1);
		TestTrue(TEXT("the triangles are the layout's"), Mesh.Primitives.Num() == 2 && Mesh.Primitives[0].Indices == TArray<uint32>({ 0, 1, 4,  0, 4, 3,  1, 2, 5,  1, 5, 4 }));
		TestTrue(TEXT("the note says a diagonal was flipped"), Note.Contains(TEXT("1 flipped")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
