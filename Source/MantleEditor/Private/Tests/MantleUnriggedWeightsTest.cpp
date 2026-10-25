// Copyright Joshua Eger. All Rights Reserved.
#include "MantleUnriggedBody.h"

#include "Misc/AutomationTest.h"
#include "Rendering/SkeletalMeshLODModel.h"

#if WITH_DEV_AUTOMATION_TESTS

// Chunk 4: a skeletal mesh is unrigged by its weights, not its bone count. Synthetic sections, no assets.

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleUnriggedWeightsTestPrivate
{
	constexpr int32 FullWeight = 65535;

	// A section of Count vertices on a skeleton of NumBones bones, its bone map the identity (local bone I is bone I). Weigh
	// sets each vertex's influences.
	FSkelMeshSection WeightsSection(const int32 NumBones, const int32 Count, TFunctionRef<void(int32 Vertex, FSoftSkinVertex& Out)> Weigh)
	{
		FSkelMeshSection Section;
		for (int32 Bone = 0; Bone < NumBones; ++Bone)
		{
			Section.BoneMap.Add(static_cast<FBoneIndexType>(Bone));
		}
		Section.SoftVertices.SetNum(Count);
		for (int32 Vertex = 0; Vertex < Count; ++Vertex)
		{
			FSoftSkinVertex& Soft = Section.SoftVertices[Vertex];
			FMemory::Memzero(Soft.InfluenceBones, sizeof(Soft.InfluenceBones));
			FMemory::Memzero(Soft.InfluenceWeights, sizeof(Soft.InfluenceWeights));
			Weigh(Vertex, Soft);
		}
		return Section;
	}

	// Vertex's influences: Share of the weight on bone A, the rest on bone B (local indices).
	void WeightsSplit(FSoftSkinVertex& Out, const int32 A, const int32 B, const double Share)
	{
		const int32 OnA = FMath::RoundToInt32(Share * FullWeight);
		Out.InfluenceBones[0] = static_cast<FBoneIndexType>(A);
		Out.InfluenceWeights[0] = static_cast<uint16>(OnA);
		Out.InfluenceBones[1] = static_cast<FBoneIndexType>(B);
		Out.InfluenceWeights[1] = static_cast<uint16>(FullWeight - OnA);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMantleUnriggedWeightsTest,
	"Mantle.Editor.UnriggedBody.NoUsableWeights",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMantleUnriggedWeightsTest::RunTest(const FString& Parameters)
{
	using namespace MantleUnriggedWeightsTestPrivate;
	const int32 Min = FMantleUnriggedBody::MinWeightedBones();
	TestEqual(TEXT("the bones a body needs weighted are the 14 the converter never inserts"), Min, 14);

	// Rigged: 60 bones, 600 vertices along a chain of 30, each vertex 70% on its bone and 30% on the next. Kept as it is.
	{
		const TArray<FSkelMeshSection> Sections = { WeightsSection(60, 600, [](const int32 Vertex, FSoftSkinVertex& Out)
		{
			const int32 Bone = Vertex % 30;
			WeightsSplit(Out, Bone, Bone + 1, 0.7);
		}) };
		const FMantleSkinWeightRead Read = FMantleUnriggedBody::ReadSkinWeights(Sections, 60);
		TestEqual(TEXT("rigged: every vertex read"), Read.Vertices, 600);
		TestEqual(TEXT("rigged: 31 bones weighted"), Read.WeightedBones, 31);
		TestEqual(TEXT("rigged: 30 bones dominant"), Read.DominantBones, 30);
		TestFalse(TEXT("a rigged mesh has usable weights"), FMantleUnriggedBody::HasNoUsableWeights(Read));
	}

	// A skeleton, every vertex wholly on its root (an export with the weights stripped): unrigged.
	{
		const TArray<FSkelMeshSection> Sections = { WeightsSection(60, 600, [](int32, FSoftSkinVertex& Out)
		{
			Out.InfluenceBones[0] = 0;
			Out.InfluenceWeights[0] = static_cast<uint16>(FullWeight);
		}) };
		const FMantleSkinWeightRead Read = FMantleUnriggedBody::ReadSkinWeights(Sections, 60);
		TestEqual(TEXT("weightless: one bone weighted"), Read.WeightedBones, 1);
		TestEqual(TEXT("weightless: one bone dominant"), Read.DominantBones, 1);
		TestTrue(TEXT("a mesh wholly on its root has no usable weights"), FMantleUnriggedBody::HasNoUsableWeights(Read));
	}

	// Every vertex mostly on one bone, the rest spread over 20 others: still one dominant bone, so unrigged.
	{
		const TArray<FSkelMeshSection> Sections = { WeightsSection(60, 600, [](const int32 Vertex, FSoftSkinVertex& Out)
		{
			WeightsSplit(Out, 3, 4 + Vertex % 20, 0.6);
		}) };
		const FMantleSkinWeightRead Read = FMantleUnriggedBody::ReadSkinWeights(Sections, 60);
		TestEqual(TEXT("one dominant: 21 bones weighted"), Read.WeightedBones, 21);
		TestEqual(TEXT("one dominant: one bone dominant"), Read.DominantBones, 1);
		TestTrue(TEXT("one dominant bone for every vertex is no usable weights"), FMantleUnriggedBody::HasNoUsableWeights(Read));
	}

	// Too few bones for a body: 6 bones weighted, each dominant somewhere. Unrigged; at 14 it would not be.
	{
		auto Chain = [](const int32 Bones)
		{
			return TArray<FSkelMeshSection>{ WeightsSection(60, 600, [Bones](const int32 Vertex, FSoftSkinVertex& Out)
			{
				Out.InfluenceBones[0] = static_cast<FBoneIndexType>(Vertex % Bones);
				Out.InfluenceWeights[0] = static_cast<uint16>(FullWeight);
			}) };
		};
		const FMantleSkinWeightRead Few = FMantleUnriggedBody::ReadSkinWeights(Chain(6), 60);
		TestEqual(TEXT("few: 6 bones weighted"), Few.WeightedBones, 6);
		TestTrue(TEXT("6 weighted bones can't carry a body"), FMantleUnriggedBody::HasNoUsableWeights(Few));
		TestTrue(TEXT("13 weighted bones can't carry a body"), FMantleUnriggedBody::HasNoUsableWeights(FMantleUnriggedBody::ReadSkinWeights(Chain(Min - 1), 60)));
		TestFalse(TEXT("14 weighted bones can"), FMantleUnriggedBody::HasNoUsableWeights(FMantleUnriggedBody::ReadSkinWeights(Chain(Min), 60)));
	}

	// Sections' bone maps: local bone 0 is bone 10 in one section and bone 20 in the other, so the mesh has two dominant bones,
	// not one; a local index outside the map, or a bone past the skeleton, counts for nothing.
	{
		auto OnLocalZero = [](int32, FSoftSkinVertex& Out)
		{
			Out.InfluenceBones[0] = 0;
			Out.InfluenceWeights[0] = static_cast<uint16>(FullWeight);
		};
		TArray<FSkelMeshSection> Sections = { WeightsSection(1, 10, OnLocalZero), WeightsSection(1, 10, OnLocalZero) };
		Sections[0].BoneMap[0] = 10;
		Sections[1].BoneMap[0] = 20;
		const FMantleSkinWeightRead Read = FMantleUnriggedBody::ReadSkinWeights(Sections, 30);
		TestEqual(TEXT("bone maps: both sections read"), Read.Vertices, 20);
		TestEqual(TEXT("bone maps: two bones dominant"), Read.DominantBones, 2);
		TestEqual(TEXT("bone maps: two bones weighted"), Read.WeightedBones, 2);
		TestEqual(TEXT("a bone past the skeleton counts for nothing"), FMantleUnriggedBody::ReadSkinWeights(Sections, 15).WeightedBones, 1);
		TArray<FSkelMeshSection> Unmapped = { WeightsSection(1, 10, [](int32, FSoftSkinVertex& Out)
		{
			Out.InfluenceBones[0] = 5;
			Out.InfluenceWeights[0] = static_cast<uint16>(FullWeight);
		}) };
		const FMantleSkinWeightRead Outside = FMantleUnriggedBody::ReadSkinWeights(Unmapped, 30);
		TestEqual(TEXT("an influence outside the bone map counts for nothing"), Outside.WeightedBones, 0);
		TestTrue(TEXT("vertices weighted to nothing usable have no usable weights"), FMantleUnriggedBody::HasNoUsableWeights(Outside));
	}

	// Nothing read: nothing to decide on, so not called unrigged.
	TestFalse(TEXT("no vertices is not called unrigged"), FMantleUnriggedBody::HasNoUsableWeights(FMantleUnriggedBody::ReadSkinWeights({}, 60)));
	TestFalse(TEXT("no skeleton reads nothing"), FMantleUnriggedBody::HasNoUsableWeights(
		FMantleUnriggedBody::ReadSkinWeights(TArray<FSkelMeshSection>{ WeightsSection(1, 10, [](int32, FSoftSkinVertex&) {}) }, 0)));

	TestEqual(TEXT("the message"), FMantleUnriggedBody::NoUsableWeightsMessage(),
		FString(TEXT("Skeleton found, but no usable weights: we'll fit it to a MetaHuman body.")));
	return true;
}

#endif
