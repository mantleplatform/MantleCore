// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarPropsFixer.h"

#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionDepthFade.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "MeshDescription.h"
#include "Misc/AutomationTest.h"
#include "SkeletalMeshAttributes.h"

#if WITH_DEV_AUTOMATION_TESTS

// #46: which bones are hand-held props (the Paragon heroes' rigs, cut down) and which slots only drive cloth. Pure; no assets.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleAvatarPropBonesTest,
	"Mantle.Editor.AvatarProps.PropBones",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleAvatarPropBonesTest::RunTest(const FString& Parameters)
{
	TArray<FName> Names;
	TArray<int32> Parents;
	auto Add = [&Names, &Parents](const TCHAR* Name, const TCHAR* Parent)
	{
		Parents.Add(Parent ? Names.IndexOfByKey(FName(Parent)) : INDEX_NONE);
		Names.Add(FName(Name));
	};
	Add(TEXT("root"), nullptr);
	Add(TEXT("pelvis"), TEXT("root"));
	Add(TEXT("spine_03"), TEXT("pelvis"));
	Add(TEXT("clavicle_r"), TEXT("spine_03"));
	Add(TEXT("upperarm_r"), TEXT("clavicle_r"));
	Add(TEXT("lowerarm_r"), TEXT("upperarm_r"));
	Add(TEXT("hand_r"), TEXT("lowerarm_r"));
	// Wraith: the rifle on the hand, its parts under it.
	Add(TEXT("weapon_r"), TEXT("hand_r"));
	Add(TEXT("gun_scope"), TEXT("weapon_r"));
	Add(TEXT("gun_scope_lens"), TEXT("gun_scope"));
	// Kallari: wrist blades on the forearm, a part with no prop word of its own under one.
	Add(TEXT("dagger_base_r"), TEXT("lowerarm_r"));
	Add(TEXT("sword_handle_r"), TEXT("dagger_base_r"));
	Add(TEXT("pivotA_r"), TEXT("sword_handle_r"));
	Add(TEXT("lowerArm_topPlateA_r"), TEXT("lowerarm_r"));
	// Murdock: the robot hand is arm, the gun on it a prop; the holstered shotgun on the back is outfit.
	Add(TEXT("bot_hand"), TEXT("hand_r"));
	Add(TEXT("gun"), TEXT("bot_hand"));
	Add(TEXT("shotgun_base"), TEXT("spine_03"));
	Add(TEXT("shotgun"), TEXT("shotgun_base"));
	// Wraith: the shoulder cannon on its mount (canon_arms is no arm).
	Add(TEXT("canon_base"), TEXT("spine_03"));
	Add(TEXT("canon_arms"), TEXT("canon_base"));
	Add(TEXT("canon_gun"), TEXT("canon_arms"));
	// Feng Mao: the pipe in his mouth; a helmet crest with a prop word stays; the hip ornaments stay.
	Add(TEXT("neck_01"), TEXT("spine_03"));
	Add(TEXT("head"), TEXT("neck_01"));
	Add(TEXT("jaw"), TEXT("head"));
	Add(TEXT("pipe"), TEXT("jaw"));
	Add(TEXT("pipe_bowl"), TEXT("pipe"));
	Add(TEXT("helmet_blade"), TEXT("head"));
	Add(TEXT("hip_eyelet_front_l_01"), TEXT("pelvis"));
	// Wukong: shoulder armour named after the shoulder blade, under the upper arm.
	Add(TEXT("shoulder_blade_out_r"), TEXT("upperarm_r"));
	// Riktor (owner 2026-10-14: a chain held in the hand is a weapon): chains named after the arm and hand they hang from.
	Add(TEXT("hand_l"), TEXT("lowerarm_r"));
	Add(TEXT("l_hand_chain_01"), TEXT("hand_l"));
	Add(TEXT("l_hand_chain_02"), TEXT("l_hand_chain_01"));
	Add(TEXT("arm_chain_long_r_01"), TEXT("hand_r"));
	Add(TEXT("arm_chain_long_r_08_sub_03"), TEXT("arm_chain_long_r_01"));
	// A chain worn on the body stays (a belt chain off the pelvis).
	Add(TEXT("belt_chain_01"), TEXT("pelvis"));
	// Kwang: a sword mounted on the back.
	Add(TEXT("weapon_backMount_r"), TEXT("spine_03"));
	// The mannequin's IK bone for a gun hangs off root, not an arm.
	Add(TEXT("ik_hand_gun"), TEXT("root"));

	const TArray<bool> Prop = FMantleAvatarPropsFixer::PropBones(Names, Parents);
	auto Is = [&Names, &Prop](const TCHAR* Name) { const int32 Index = Names.IndexOfByKey(FName(Name)); return Index != INDEX_NONE && Prop[Index]; };
	for (const TCHAR* Held : { TEXT("weapon_r"), TEXT("gun_scope"), TEXT("gun_scope_lens"), TEXT("dagger_base_r"), TEXT("sword_handle_r"), TEXT("pivotA_r"), TEXT("gun"), TEXT("pipe"), TEXT("pipe_bowl"), TEXT("l_hand_chain_01"),
		TEXT("l_hand_chain_02"), TEXT("arm_chain_long_r_01"), TEXT("arm_chain_long_r_08_sub_03") })
	{
		TestTrue(FString::Printf(TEXT("%s is a held prop"), Held), Is(Held));
	}
	for (const TCHAR* Kept : { TEXT("root"), TEXT("spine_03"), TEXT("upperarm_r"), TEXT("hand_r"), TEXT("lowerArm_topPlateA_r"), TEXT("bot_hand"), TEXT("shotgun_base"),
		TEXT("shotgun"), TEXT("canon_gun"), TEXT("weapon_backMount_r"), TEXT("ik_hand_gun"), TEXT("head"), TEXT("jaw"),
		TEXT("helmet_blade"), TEXT("hip_eyelet_front_l_01"), TEXT("shoulder_blade_out_r"), TEXT("belt_chain_01"), TEXT("hand_l") })
	{
		TestFalse(FString::Printf(TEXT("%s is not a held prop"), Kept), Is(Kept));
	}

	// Serath's wings: translucent at opacity 0 until her ability shows them; a masked section under its clip value.
	TestTrue(TEXT("translucent at 0 is hidden"), FMantleAvatarPropsFixer::IsHidden(false, 0.0f, 0.333f));
	TestFalse(TEXT("faint translucent is shown"), FMantleAvatarPropsFixer::IsHidden(false, 0.05f, 0.333f));
	TestTrue(TEXT("masked under its clip is hidden"), FMantleAvatarPropsFixer::IsHidden(true, 0.2f, 0.333f));
	TestFalse(TEXT("masked reaching its clip is shown"), FMantleAvatarPropsFixer::IsHidden(true, 0.5f, 0.333f));

	TestTrue(TEXT("Cloth_Sim_Mat drives cloth"), FMantleAvatarPropsFixer::IsClothSimSlot(TEXT("Cloth_Sim_Mat")));
	TestTrue(TEXT("ClothSim drives cloth"), FMantleAvatarPropsFixer::IsClothSimSlot(TEXT("ClothSim")));
	TestFalse(TEXT("Silk_Cloth is rendered cloth"), FMantleAvatarPropsFixer::IsClothSimSlot(TEXT("Silk_Cloth")));
	TestFalse(TEXT("M01_Torso is not cloth"), FMantleAvatarPropsFixer::IsClothSimSlot(TEXT("M01_Torso")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleAvatarKnownOpacityTest,
	"Mantle.Editor.AvatarProps.KnownOpacity",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

// Serath's wing graph cut down: opacity = DepthFade(lerp(0, texture x 0.8, texture) x WingAlpha). Hidden while WingAlpha
// is 0, unknown (shown) once it isn't; a texture alone proves nothing.
bool FMantleAvatarKnownOpacityTest::RunTest(const FString& Parameters)
{
	UMaterial* Material = NewObject<UMaterial>(GetTransientPackage());
	Material->BlendMode = BLEND_Translucent;
	UMaterialExpressionTextureSample* Texture = NewObject<UMaterialExpressionTextureSample>(Material);
	UMaterialExpressionMultiply* Strength = NewObject<UMaterialExpressionMultiply>(Material);
	Strength->A.Expression = Texture;
	Strength->ConstB = 0.8f;
	UMaterialExpressionLinearInterpolate* Lerp = NewObject<UMaterialExpressionLinearInterpolate>(Material);
	Lerp->ConstA = 0.0f;
	Lerp->B.Expression = Strength;
	Lerp->Alpha.Expression = Texture;
	UMaterialExpressionScalarParameter* WingAlpha = NewObject<UMaterialExpressionScalarParameter>(Material);
	WingAlpha->ParameterName = TEXT("WingAlpha");
	WingAlpha->DefaultValue = 0.0f;
	UMaterialExpressionMultiply* Gate = NewObject<UMaterialExpressionMultiply>(Material);
	Gate->A.Expression = Lerp;
	Gate->B.Expression = WingAlpha;
	UMaterialExpressionDepthFade* Fade = NewObject<UMaterialExpressionDepthFade>(Material);
	Fade->InOpacity.Expression = Gate;
	Material->GetEditorOnlyData()->Opacity.Expression = Fade;

	const TOptional<float> Hidden = FMantleAvatarPropsFixer::KnownOpacity(Material);
	TestTrue(TEXT("WingAlpha 0: the graph proves opacity 0"), Hidden.IsSet() && Hidden.GetValue() == 0.0f);
	WingAlpha->DefaultValue = 1.0f;
	TestFalse(TEXT("WingAlpha 1: nothing is proven"), FMantleAvatarPropsFixer::KnownOpacity(Material).IsSet());
	Material->GetEditorOnlyData()->Opacity.Expression = Texture;
	TestFalse(TEXT("a texture alone proves nothing"), FMantleAvatarPropsFixer::KnownOpacity(Material).IsSet());
	UMaterialExpressionConstant* Half = NewObject<UMaterialExpressionConstant>(Material);
	Half->R = 0.5f;
	Material->GetEditorOnlyData()->Opacity.Expression = Half;
	const TOptional<float> Constant = FMantleAvatarPropsFixer::KnownOpacity(Material);
	TestTrue(TEXT("a constant 0.5 reads 0.5, shown"), Constant.IsSet() && Constant.GetValue() == 0.5f
		&& !FMantleAvatarPropsFixer::IsHidden(false, Constant.GetValue(), 0.333f));
	Material->BlendMode = BLEND_Opaque;
	TestFalse(TEXT("an opaque material is never read"), FMantleAvatarPropsFixer::KnownOpacity(Material).IsSet());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleAvatarPropsLowerLODTest,
	"Mantle.Editor.AvatarProps.LowerLOD",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

// Feng Mao's LOD 2: the pipe skinned to "head" (its bone dropped), so only where LOD 0's pipe was can find it. Two triangles
// on "head": one where LOD 0 kept the face, one where LOD 0's pipe went. Only the second goes.
bool FMantleAvatarPropsLowerLODTest::RunTest(const FString& Parameters)
{
	FMeshDescription Description;
	FSkeletalMeshAttributes Attributes(Description);
	Attributes.Register();
	const FPolygonGroupID Group = Description.CreatePolygonGroup();
	Attributes.GetPolygonGroupMaterialSlotNames().Set(Group, TEXT("M00_Head"));
	FSkinWeightsVertexAttributesRef Weights = Attributes.GetVertexSkinWeights();
	const UE::AnimationCore::FBoneWeight OnHead(static_cast<FBoneIndexType>(1), 1.0f);
	auto Triangle = [&](const FVector3f& At)
	{
		TArray<FVertexInstanceID> Corners;
		for (const FVector3f& Offset : { FVector3f(0, 0, 0), FVector3f(1, 0, 0), FVector3f(0, 0, 1) })
		{
			const FVertexID Vertex = Description.CreateVertex();
			Attributes.GetVertexPositions().Set(Vertex, At + Offset);
			Weights.Set(Vertex, UE::AnimationCore::FBoneWeights::Create(MakeArrayView(&OnHead, 1)));
			Corners.Add(Description.CreateVertexInstance(Vertex));
		}
		Description.CreatePolygon(Group, Corners);
	};
	Triangle(FVector3f(0, 5, 150));
	Triangle(FVector3f(0, 30, 143));

	const TArray<FName> Names = { TEXT("root"), TEXT("head") };
	const TArray<int32> Parents = { INDEX_NONE, 0 };
	FMantleAvatarPropsFixer::FPropCut Reference;
	Reference.KeptPoints = { FVector3f(0, 5, 150), FVector3f(0.5f, 5, 150.5f) };
	Reference.RemovedPoints = { FVector3f(0, 30, 143), FVector3f(0.5f, 30, 143.5f) };

	FMantleAvatarPropsFixer::FPropCut Cut;
	TestTrue(TEXT("the LOD is read"), FMantleAvatarPropsFixer::CutProps(Description, Names, Parents, /*bDelete*/ true, Cut, &Reference));
	TestEqual(TEXT("one triangle goes, where LOD 0's prop was"), Cut.LooseTriangles, 1);
	TestEqual(TEXT("the face's triangle stays"), Description.Triangles().Num(), 1);
	float MaxY = -1.0f;
	for (const FVertexID Vertex : Description.Vertices().GetElementIDs())
	{
		MaxY = FMath::Max(MaxY, Attributes.GetVertexPositions().Get(Vertex).Y);
	}
	TestTrue(TEXT("no point is left where the prop was"), MaxY < 10.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleAvatarPropsShellTest,
	"Mantle.Editor.AvatarProps.ShellOverBody",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

// mantle#861's fault: an invisible shell 1 cm over the legs went from LOD 0, and LOD 1's reduced legs (their points moved
// half a centimetre towards it) went with it. A point goes only when it is clearly nearer what went than what stayed.
bool FMantleAvatarPropsShellTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("on the removed shell, the body 1 cm away: goes"), FMantleAvatarPropsFixer::IsWhereRemoved(TOptional<float>(0.0f), TOptional<float>(1.0f)));
	TestFalse(TEXT("between shell and leg: stays"), FMantleAvatarPropsFixer::IsWhereRemoved(TOptional<float>(0.5f), TOptional<float>(0.5f)));
	TestFalse(TEXT("a little nearer the shell: stays"), FMantleAvatarPropsFixer::IsWhereRemoved(TOptional<float>(0.3f), TOptional<float>(0.8f)));
	TestTrue(TEXT("nothing kept in reach: goes"), FMantleAvatarPropsFixer::IsWhereRemoved(TOptional<float>(4.0f), TOptional<float>()));
	TestFalse(TEXT("nothing removed in reach: stays"), FMantleAvatarPropsFixer::IsWhereRemoved(TOptional<float>(), TOptional<float>(3.0f)));
	TestFalse(TEXT("removed beyond reach: stays"), FMantleAvatarPropsFixer::IsWhereRemoved(TOptional<float>(7.0f), TOptional<float>()));

	// A LOD 1 leg triangle 0.5 cm under LOD 0's removed shell and 0.5 cm over its kept leg; and a shell triangle on the shell.
	FMeshDescription Description;
	FSkeletalMeshAttributes Attributes(Description);
	Attributes.Register();
	const FPolygonGroupID Group = Description.CreatePolygonGroup();
	Attributes.GetPolygonGroupMaterialSlotNames().Set(Group, TEXT("M03_Legs"));
	FSkinWeightsVertexAttributesRef Weights = Attributes.GetVertexSkinWeights();
	const UE::AnimationCore::FBoneWeight OnThigh(static_cast<FBoneIndexType>(1), 1.0f);
	auto Triangle = [&](const float Y)
	{
		TArray<FVertexInstanceID> Corners;
		for (const FVector3f& Offset : { FVector3f(0, 0, 0), FVector3f(1, 0, 0), FVector3f(0, 0, 1) })
		{
			const FVertexID Vertex = Description.CreateVertex();
			Attributes.GetVertexPositions().Set(Vertex, FVector3f(0, Y, 80) + Offset);
			Weights.Set(Vertex, UE::AnimationCore::FBoneWeights::Create(MakeArrayView(&OnThigh, 1)));
			Corners.Add(Description.CreateVertexInstance(Vertex));
		}
		Description.CreatePolygon(Group, Corners);
	};
	Triangle(10.5f);
	Triangle(11.0f);

	FMantleAvatarPropsFixer::FPropCut Reference;
	for (const FVector3f& Offset : { FVector3f(0, 0, 0), FVector3f(1, 0, 0), FVector3f(0, 0, 1) })
	{
		Reference.KeptPoints.Add(FVector3f(0, 10, 80) + Offset);
		Reference.RemovedPoints.Add(FVector3f(0, 11, 80) + Offset);
	}
	const TArray<FName> Names = { TEXT("root"), TEXT("thigh_l") };
	const TArray<int32> Parents = { INDEX_NONE, 0 };
	FMantleAvatarPropsFixer::FPropCut Cut;
	TestTrue(TEXT("the LOD is read"), FMantleAvatarPropsFixer::CutProps(Description, Names, Parents, /*bDelete*/ true, Cut, &Reference));
	TestEqual(TEXT("only the shell's triangle goes"), Cut.LooseTriangles, 1);
	TestEqual(TEXT("the leg's triangle stays"), Description.Triangles().Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleAvatarPropsMorphsTest,
	"Mantle.Editor.AvatarProps.MorphsSurvive",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

// The issue: morph targets were lost in the proof of concept (Terra's two). Cutting a sword from the hand keeps the face's
// morph and its deltas on the points that stay; LostMorphs names what a copy lacks.
bool FMantleAvatarPropsMorphsTest::RunTest(const FString& Parameters)
{
	TArray<FName> Names;
	TArray<int32> Parents;
	auto Add = [&Names, &Parents](const TCHAR* Name, const TCHAR* Parent)
	{
		Parents.Add(Parent ? Names.IndexOfByKey(FName(Parent)) : INDEX_NONE);
		Names.Add(FName(Name));
	};
	Add(TEXT("root"), nullptr);
	Add(TEXT("lowerarm_r"), TEXT("root"));
	Add(TEXT("hand_r"), TEXT("lowerarm_r"));
	Add(TEXT("sword_r"), TEXT("hand_r"));
	Add(TEXT("head"), TEXT("root"));

	FMeshDescription Description;
	FSkeletalMeshAttributes Attributes(Description);
	Attributes.Register();
	const FName Smile(TEXT("Smile"));
	Attributes.RegisterMorphTargetAttribute(Smile, /*bIncludeNormals*/ false);
	const FPolygonGroupID Body = Description.CreatePolygonGroup();
	Attributes.GetPolygonGroupMaterialSlotNames().Set(Body, TEXT("M01_Body"));
	const FPolygonGroupID Sword = Description.CreatePolygonGroup();
	Attributes.GetPolygonGroupMaterialSlotNames().Set(Sword, TEXT("M09_Sword"));
	FSkinWeightsVertexAttributesRef Weights = Attributes.GetVertexSkinWeights();
	auto Triangle = [&](const FPolygonGroupID Group, const int32 Bone, const FVector3f& At, const FVector3f& Delta)
	{
		const UE::AnimationCore::FBoneWeight On(static_cast<FBoneIndexType>(Bone), 1.0f);
		TArray<FVertexInstanceID> Corners;
		for (const FVector3f& Offset : { FVector3f(0, 0, 0), FVector3f(1, 0, 0), FVector3f(0, 0, 1) })
		{
			const FVertexID Vertex = Description.CreateVertex();
			Attributes.GetVertexPositions().Set(Vertex, At + Offset);
			Attributes.GetVertexMorphPositionDelta(Smile).Set(Vertex, Delta);
			Weights.Set(Vertex, UE::AnimationCore::FBoneWeights::Create(MakeArrayView(&On, 1)));
			Corners.Add(Description.CreateVertexInstance(Vertex));
		}
		Description.CreatePolygon(Group, Corners);
	};
	// The sword first, so the face's points move when the description is compacted.
	Triangle(Sword, 3, FVector3f(60, 0, 100), FVector3f::ZeroVector);
	Triangle(Sword, 3, FVector3f(70, 0, 100), FVector3f::ZeroVector);
	Triangle(Body, 4, FVector3f(0, 10, 160), FVector3f(0, 0.5f, 0));

	FMantleAvatarPropsFixer::FPropCut Cut;
	TestTrue(TEXT("the LOD is read"), FMantleAvatarPropsFixer::CutProps(Description, Names, Parents, /*bDelete*/ true, Cut));
	TestEqual(TEXT("the sword's section goes"), Cut.Sections.Num(), 1);
	TestEqual(TEXT("the face's triangle stays"), Description.Triangles().Num(), 1);
	FSkeletalMeshAttributes After(Description);
	TestTrue(TEXT("the morph is still there"), After.GetMorphTargetNames().Contains(Smile));
	TVertexAttributesRef<FVector3f> Deltas = After.GetVertexMorphPositionDelta(Smile);
	bool bDeltas = Deltas.IsValid() && Description.Vertices().Num() == 3;
	for (const FVertexID Vertex : Description.Vertices().GetElementIDs())
	{
		bDeltas &= Deltas.IsValid() && Deltas.Get(Vertex).Equals(FVector3f(0, 0.5f, 0));
	}
	TestTrue(TEXT("the face keeps its morph deltas"), bDeltas);

	const TArray<FName> Before = { FName(TEXT("Smile")), FName(TEXT("Blink")) };
	const TArray<FName> Kept = { FName(TEXT("Smile")) };
	const TArray<FName> More = { FName(TEXT("Smile")), FName(TEXT("Extra")) };
	const TArray<FName> Lost = FMantleAvatarPropsFixer::LostMorphs(Before, Kept);
	TestEqual(TEXT("one morph lost"), Lost.Num(), 1);
	TestTrue(TEXT("it is Blink"), Lost.Num() == 1 && Lost[0] == FName(TEXT("Blink")));
	TestEqual(TEXT("none lost"), FMantleAvatarPropsFixer::LostMorphs(Kept, More).Num(), 0);
	return true;
}

#endif
