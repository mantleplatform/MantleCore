// Copyright Joshua Eger. All Rights Reserved.
#include "MantleFitBodies.h"

#include "MantleAvatarContract.h"
#include "MantleBodyCorrespondence.h"
#include "MantleBodyTopologyData.h"
#include "MantleCosmeticExport.h"
#include "MantleParametricOutfit.h"

#include "Async/Async.h"
#include "Editor.h"
#include "MeshDescription.h"
#include "HAL/IConsoleManager.h"
#include "MetaHumanCharacterBodyIdentity.h"
#include "MetaHumanCharacterEditorSubsystem.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/StaticMeshVertexBuffer.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "SkeletalMeshAttributes.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleFitBodies, Log, All);

// The body type values are Epic's enum order; the table below is built from them, so a reorder would break the build here.
static_assert(static_cast<int32>(EMetaHumanBodyType::f_med_nrw) == 0 && static_cast<int32>(EMetaHumanBodyType::f_srt_nrw) == 3
	&& static_cast<int32>(EMetaHumanBodyType::f_tal_nrw) == 6 && static_cast<int32>(EMetaHumanBodyType::m_med_nrw) == 9
	&& static_cast<int32>(EMetaHumanBodyType::m_tal_unw) == 17, "EMetaHumanBodyType order changed");

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleFitBodiesPrivate
{
	/** Sets mh.Character.LoadMeshesFromDNA while it lives (Creator then makes face and body meshes from the DNA archetypes, as the build does). */
	struct FScopedDnaArchetypeMeshes
	{
		IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("mh.Character.LoadMeshesFromDNA"));
		bool bPrevious = false;
		FScopedDnaArchetypeMeshes()
		{
			if (Variable) { bPrevious = Variable->GetBool(); Variable->Set(true, ECVF_SetByCode); }
			else { UE_LOG(LogMantleFitBodies, Warning, TEXT("mh.Character.LoadMeshesFromDNA is not there; the fit bodies' faces may be numbered unlike built faces")); }
		}
		~FScopedDnaArchetypeMeshes() { if (Variable) { Variable->Set(bPrevious, ECVF_SetByCode); } }
	};
	FCriticalSection CreatorPreviewLock;
	TSet<TWeakObjectPtr<const USkeletalMesh>>& CreatorPreviews()
	{
		static TSet<TWeakObjectPtr<const USkeletalMesh>> Meshes;
		return Meshes;
	}

	/** The preview body that goes with each preview face (under CreatorPreviewLock). */
	TMap<TWeakObjectPtr<const USkeletalMesh>, TWeakObjectPtr<const USkeletalMesh>>& FaceBodies()
	{
		static TMap<TWeakObjectPtr<const USkeletalMesh>, TWeakObjectPtr<const USkeletalMesh>> Map;
		return Map;
	}

	UMetaHumanCharacterEditorSubsystem* GetSubsystem()
	{
		return GEditor ? GEditor->GetEditorSubsystem<UMetaHumanCharacterEditorSubsystem>() : nullptr;
	}

	/** srt|med|tal then nrw|ovw|unw as the offsets within one sex's nine bodies; false for anything else. */
	bool ParseSuffix(const FString& Suffix, int32& OutOffset)
	{
		TArray<FString> Parts;
		Suffix.ParseIntoArray(Parts, TEXT("_"));
		if (Parts.Num() != 2)
		{
			return false;
		}
		int32 HeightBase = INDEX_NONE;
		// Case-sensitive: FString's == ignores case, and the ids are lowercase.
		if (Parts[0].Equals(TEXT("med"), ESearchCase::CaseSensitive)) HeightBase = 0;
		else if (Parts[0].Equals(TEXT("srt"), ESearchCase::CaseSensitive)) HeightBase = 3;
		else if (Parts[0].Equals(TEXT("tal"), ESearchCase::CaseSensitive)) HeightBase = 6;
		int32 BuildOffset = INDEX_NONE;
		if (Parts[1].Equals(TEXT("nrw"), ESearchCase::CaseSensitive)) BuildOffset = 0;
		else if (Parts[1].Equals(TEXT("ovw"), ESearchCase::CaseSensitive)) BuildOffset = 1;
		else if (Parts[1].Equals(TEXT("unw"), ESearchCase::CaseSensitive)) BuildOffset = 2;
		if (HeightBase == INDEX_NONE || BuildOffset == INDEX_NONE)
		{
			return false;
		}
		OutOffset = HeightBase + BuildOffset;
		return true;
	}

	/** The nearest-vertex average distance of an Outfit size's body parts to a reference body; negative when unreadable. */
	float SizeDistanceCm(const FMantleOutfitSourceSize& Size, const TArray<FVector3f>& Reference)
	{
		TArray<FVector3f> Probe;
		for (const FSoftObjectPath& Path : Size.BodyParts)
		{
			const USkeletalMesh* Part = Cast<USkeletalMesh>(Path.TryLoad());
			TArray<FVector3f> Vertices;
			if (Part && FMantleFitBodies::ReadVertices(Part, 0, Vertices))
			{
				Probe.Append(Vertices);
			}
		}
		if (Probe.IsEmpty() || Reference.IsEmpty())
		{
			return -1.0f;
		}
		// A few thousand points show where the body is; every vertex would only make the comparison slower.
		constexpr int32 MaxProbe = 6000;
		if (Probe.Num() > MaxProbe)
		{
			TArray<FVector3f> Thinned;
			const int32 Stride = FMath::DivideAndRoundUp(Probe.Num(), MaxProbe);
			for (int32 Index = 0; Index < Probe.Num(); Index += Stride)
			{
				Thinned.Add(Probe[Index]);
			}
			Probe = MoveTemp(Thinned);
		}
		float Average = 0.0f, Largest = 0.0f;
		return FMantleFitBodies::CompareNearest(Probe, Reference, Average, Largest) ? Average : -1.0f;
	}

	bool BodyVertices(const FMantleFitBody& Body, TArray<FVector3f>& Out)
	{
		const USkeletalMesh* Mesh = Body.MergedHeadAndBodyMesh.IsValid() ? Body.MergedHeadAndBodyMesh.Get() : Body.BodyMesh.Get();
		return FMantleFitBodies::ReadVertices(Mesh, 0, Out);
	}
}

FMantleFitBodySet::~FMantleFitBodySet()
{
	if (Bodies.IsEmpty())
	{
		return;
	}
	if (IsInGameThread())
	{
		for (FMantleFitBody& Body : Bodies)
		{
			FMantleFitBodies::Release(Body);
		}
		return;
	}
	AsyncTask(ENamedThreads::GameThread, [Moved = MoveTemp(Bodies)]() mutable
	{
		for (FMantleFitBody& Body : Moved)
		{
			FMantleFitBodies::Release(Body);
		}
	});
}

bool FMantleFitBodies::ParseBodyType(const FString& Name, EMetaHumanBodyType& OutType)
{
	FString Sex, Suffix;
	if (!Name.Split(TEXT("_"), &Sex, &Suffix) || (!Sex.Equals(TEXT("f"), ESearchCase::CaseSensitive) && !Sex.Equals(TEXT("m"), ESearchCase::CaseSensitive)))
	{
		return false;
	}
	int32 Offset = 0;
	if (!MantleFitBodiesPrivate::ParseSuffix(Suffix, Offset))
	{
		return false;
	}
	OutType = static_cast<EMetaHumanBodyType>((Sex.Equals(TEXT("m"), ESearchCase::CaseSensitive) ? 9 : 0) + Offset);
	return true;
}

bool FMantleFitBodies::IsBodyTypeSuffix(const FString& Suffix)
{
	int32 Offset = 0;
	return MantleFitBodiesPrivate::ParseSuffix(Suffix, Offset);
}

TArray<FString> FMantleFitBodies::SanitizeBodyTypes(const TArray<FString>& Raw, const int32 MaxVariants)
{
	TArray<FString> Out;
	for (const FString& Entry : Raw)
	{
		if (Out.Num() < MaxVariants && IsBodyTypeSuffix(Entry))
		{
			Out.AddUnique(Entry);
		}
	}
	return Out;
}

TArray<FString> FMantleFitBodies::VariantIds(const EMantleFitSex Sex, const TArray<FString>& Suffixes)
{
	TArray<FString> Out;
	for (const FString& Suffix : Suffixes)
	{
		Out.Add(FString::Printf(TEXT("%s_%s"), Sex == EMantleFitSex::Male ? TEXT("m") : TEXT("f"), *Suffix));
	}
	return Out;
}

EMantleFitSex FMantleFitBodies::ChooseSex(const TArray<float>& FemaleCm, const TArray<float>& MaleCm, const float MarginCm, FString& OutWhy)
{
	const auto Nearest = [](const TArray<float>& Distances)
	{
		float Best = -1.0f;
		for (const float Distance : Distances)
		{
			if (Distance >= 0.0f && (Best < 0.0f || Distance < Best))
			{
				Best = Distance;
			}
		}
		return Best;
	};
	const float Female = Nearest(FemaleCm);
	const float Male = Nearest(MaleCm);
	if (Female < 0.0f || Male < 0.0f)
	{
		OutWhy = TEXT("the Outfit's authored size bodies could not be compared with the standard bodies");
		return EMantleFitSex::Female;
	}
	if (Male + MarginCm < Female)
	{
		OutWhy = FString::Printf(TEXT("its authored size bodies are %.2f cm from the standard male body and %.2f cm from the female one"), Male, Female);
		return EMantleFitSex::Male;
	}
	OutWhy = Female <= Male
		? FString::Printf(TEXT("its authored size bodies are %.2f cm from the standard female body and %.2f cm from the male one"), Female, Male)
		: FString::Printf(TEXT("its authored size bodies are %.2f cm from the standard female body and %.2f cm from the male one, too close to call"), Female, Male);
	return EMantleFitSex::Female;
}

bool FMantleFitBodies::Make(const FString& Id, const EMetaHumanBodyType Type, FMantleFitBody& Out, FString& OutError)
{
	Release(Out);
	Out = FMantleFitBody();
	Out.Id = Id;

	UMetaHumanCharacterEditorSubsystem* Subsystem = MantleFitBodiesPrivate::GetSubsystem();
	if (!Subsystem)
	{
		OutError = TEXT("MetaHuman Creator's editor subsystem is unavailable");
		return false;
	}
	// The transient package: nothing is ever saved into the project.
	UObject* CharacterOuter = GetTransientPackage();
	const FName CharacterName = MakeUniqueObjectName(CharacterOuter, UMetaHumanCharacter::StaticClass(), FName(*FString::Printf(TEXT("MantleFitBody_%s"), *Id)));
	UMetaHumanCharacter* Character = NewObject<UMetaHumanCharacter>(CharacterOuter, UMetaHumanCharacter::StaticClass(), CharacterName);
	if (!Character)
	{
		OutError = TEXT("could not create the in-memory MetaHuman character");
		return false;
	}
	Out.Character.Reset(Character);
	// Epic's build makes its meshes from the DNA archetypes (TryGenerateCharacterAssets), while Creator's preview of a character that has no face DNA uses
	// the SKM_Face / SKM_Body assets, whose triangles are numbered differently: the client's collar pairs a wearer's face to the garment's by triangle index.
	// Have Creator open this character the way the build does, for the length of this function.
	MantleFitBodiesPrivate::FScopedDnaArchetypeMeshes DnaArchetypes;
	Subsystem->InitializeMetaHumanCharacter(Character);
	if (!Character->IsCharacterValid())
	{
		OutError = TEXT("MetaHuman Creator did not initialise the in-memory character");
		return false;
	}
	if (!Subsystem->TryAddObjectToEdit(Character))
	{
		OutError = TEXT("MetaHuman Creator could not open the in-memory character for editing");
		return false;
	}
	Out.bEditing = true;

	// Creator's own body-type picker does this: select the fixed body, then commit it. All local; no cloud.
	Subsystem->SetMetaHumanBodyType(Character, Type, UMetaHumanCharacterEditorSubsystem::EBodyMeshUpdateMode::Full);
	Subsystem->CommitBodyState(Character);

	FMetaHumanCharacterPreviewAssetOptions Options;
	Options.bGenerateMergedHeadAndBodyMesh = true;
	Options.bGenerateBodyMeasurements = true;
	FMetaHumanCharacterPreviewAssets Preview;
	if (!Subsystem->TryGetCharacterPreviewAssets(Character, Options, Preview) || !Preview.BodyMesh || !Preview.MergedHeadAndBodyMesh)
	{
		OutError = TEXT("MetaHuman Creator could not provide the in-memory character's body");
		return false;
	}
	Out.BodyMesh.Reset(Preview.BodyMesh.Get());
	{
		FScopeLock Lock(&MantleFitBodiesPrivate::CreatorPreviewLock);
		TSet<TWeakObjectPtr<const USkeletalMesh>>& Meshes = MantleFitBodiesPrivate::CreatorPreviews();
		for (auto It = Meshes.CreateIterator(); It; ++It) { if (!It->IsValid()) { It.RemoveCurrent(); } }
		Meshes.Add(Preview.BodyMesh.Get());
		if (Preview.FaceMesh) { Meshes.Add(Preview.FaceMesh.Get()); MantleFitBodiesPrivate::FaceBodies().Add(Preview.FaceMesh.Get(), Preview.BodyMesh.Get()); }
	}
	Out.FaceMesh.Reset(Preview.FaceMesh.Get());
	Out.MergedHeadAndBodyMesh.Reset(Preview.MergedHeadAndBodyMesh.Get());
	Out.FaceNumberingHash = FaceNumberingHash(Out.FaceMesh.Get());
	UE_LOG(LogMantleFitBodies, Display, TEXT("fit body %s made in memory"), *Out.Id);
	return true;
}

void FMantleFitBodies::Release(FMantleFitBody& Body)
{
	if (Body.bEditing && Body.Character.IsValid())
	{
		if (UMetaHumanCharacterEditorSubsystem* Subsystem = MantleFitBodiesPrivate::GetSubsystem())
		{
			Subsystem->RemoveObjectToEdit(Body.Character.Get());
		}
	}
	Body.bEditing = false;
	if (Body.BodyMesh.IsValid())
	{
		FScopeLock Lock(&MantleFitBodiesPrivate::CreatorPreviewLock);
		MantleFitBodiesPrivate::CreatorPreviews().Remove(Body.BodyMesh.Get());
		if (Body.FaceMesh.IsValid()) { MantleFitBodiesPrivate::CreatorPreviews().Remove(Body.FaceMesh.Get()); MantleFitBodiesPrivate::FaceBodies().Remove(Body.FaceMesh.Get()); }
	}
	Body.BodyMesh.Reset();
	Body.FaceMesh.Reset();
	Body.MergedHeadAndBodyMesh.Reset();
	Body.Character.Reset();
}

FString FMantleFitBodies::DescribeReferenceTopology(const USkeletalMesh* Mesh, const FMantleAvatarContract& Contract)
{
	FString Out;
	const int32 NumLODs = UMantleBodyTopologyData::GetNumLODs(Mesh);
	const FMantleBodyCorrespondence* Layout = FMantleBodyCorrespondence::Get();
	for (int32 LOD = 0; LOD < NumLODs; ++LOD)
	{
		FMantleBodyLODTopology Topology;
		TArray<FVector2f> UVs;
		FString ReadError;
		FString Line;
		if (!UMantleBodyTopologyData::ReadLOD(Mesh, LOD, Topology, nullptr, &UVs, &ReadError))
		{
			Line = FString::Printf(TEXT("LOD%d unreadable (%s)"), LOD, *ReadError);
		}
		else
		{
			Line = FString::Printf(TEXT("LOD%d %d verts, %d tris, %d sections, uvhash %08x (reference %d verts, uvhash %08x)"), LOD, Topology.VertexCount,
				Topology.TriangleCount, Topology.SectionCount, Topology.UVHash, Contract.TopologyVertexCount, Contract.TopologyUVHash);
			if (Topology.VertexCount == Contract.TopologyVertexCount)
			{
				const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
				TArray<uint32> Indices;
				if (RenderData && RenderData->LODRenderData.IsValidIndex(LOD))
				{
					RenderData->LODRenderData[LOD].MultiSizeIndexContainer.GetIndexBuffer(Indices);
				}
				FString MatchError = TEXT("no layout resource");
				TArray<int32> Map;
				if (Layout)
				{
					FString Error;
					MatchError = Layout->Match(UVs, Indices, Map, Error) ? FString(TEXT("matches")) : Error;
					MatchError += FString::Printf(TEXT(" [indices %d, reference %d]"), Indices.Num(), Layout->Indices.Num());
				}
				Line += TEXT(": Match: ") + MatchError;
			}
		}
		Out += (Out.IsEmpty() ? TEXT("") : TEXT("; ")) + Line;
	}
	return Out.IsEmpty() ? FString(TEXT("no LODs")) : Out;
}

uint32 FMantleFitBodies::FaceNumberingHash(const USkeletalMesh* Face)
{
	constexpr int32 ReferenceFaceTriangles = 6872;
	const FSkeletalMeshRenderData* RenderData = Face ? Face->GetResourceForRendering() : nullptr;
	if (!RenderData || RenderData->LODRenderData.IsEmpty())
	{
		return 0;
	}
	int32 Chosen = 0;
	for (int32 LOD = 0; LOD < RenderData->LODRenderData.Num(); ++LOD)
	{
		if (RenderData->LODRenderData[LOD].GetTotalFaces() == ReferenceFaceTriangles) { Chosen = LOD; break; }
	}
	const FSkeletalMeshLODRenderData& LOD = RenderData->LODRenderData[Chosen];
	const FStaticMeshVertexBuffer& Vertices = LOD.StaticVertexBuffers.StaticMeshVertexBuffer;
	if (!LOD.MultiSizeIndexContainer.IsIndexBufferValid() || !Vertices.GetTexCoordData() || Vertices.GetNumTexCoords() == 0)
	{
		return 0;
	}
	TArray<uint32> Indices;
	LOD.MultiSizeIndexContainer.GetIndexBuffer(Indices);
	uint32 Hash = 0;
	for (int32 First = 0; First + 2 < Indices.Num(); First += 3)
	{
		if (Indices[First] >= Vertices.GetNumVertices() || Indices[First + 1] >= Vertices.GetNumVertices() || Indices[First + 2] >= Vertices.GetNumVertices()) { return 0; }
		const FVector2f Centroid = (Vertices.GetVertexUV(Indices[First], 0) + Vertices.GetVertexUV(Indices[First + 1], 0) + Vertices.GetVertexUV(Indices[First + 2], 0)) / 3.0f;
		const int32 Quantized[2] = { FMath::RoundToInt32(Centroid.X * 1024.0f), FMath::RoundToInt32(Centroid.Y * 1024.0f) };
		Hash = FCrc::MemCrc32(Quantized, sizeof(Quantized), Hash);
	}
	return Hash == 0 ? 1 : Hash;
}

bool FMantleFitBodies::IsCreatorPreview(const USkeletalMesh* Mesh)
{
	FScopeLock Lock(&MantleFitBodiesPrivate::CreatorPreviewLock);
	return Mesh && MantleFitBodiesPrivate::CreatorPreviews().Contains(Mesh);
}

bool FMantleFitBodies::NeckBaseZ(const USkeletalMesh* Face, float& OutZ)
{
	const USkeletalMesh* Body = nullptr;
	{
		FScopeLock Lock(&MantleFitBodiesPrivate::CreatorPreviewLock);
		const TWeakObjectPtr<const USkeletalMesh>* Found = Face ? MantleFitBodiesPrivate::FaceBodies().Find(Face) : nullptr;
		Body = Found ? Found->Get() : nullptr;
	}
	const FMantleAvatarContract* Contract = FMantleAvatarContract::Get();
	if (!Body || !Contract)
	{
		return false;
	}
	for (int32 LOD = 0; LOD < UMantleBodyTopologyData::GetNumLODs(Body); ++LOD)
	{
		FMantleBodyLODTopology Topology;
		TArray<FVector3f> Positions;
		if (UMantleBodyTopologyData::ReadLOD(Body, LOD, Topology, &Positions) && Topology.VertexCount == Contract->TopologyVertexCount)
		{
			float Base = -MAX_flt;
			for (const FVector3f& P : Positions) { if (P.X * P.X + P.Y * P.Y <= 12.0f * 12.0f) { Base = FMath::Max(Base, P.Z); } }
			OutZ = Base;
			return Base > -MAX_flt;
		}
	}
	return false;
}

USkeletalMesh* FMantleFitBodies::PickShapeBody(const FMantleFitBody& Body, const FMantleAvatarContract& Contract, FString& OutLog, FString* OutOrdering)
{
	const auto Counts = [](const USkeletalMesh* Mesh)
	{
		FString Text;
		const FSkeletalMeshRenderData* RenderData = Mesh ? Mesh->GetResourceForRendering() : nullptr;
		if (RenderData)
		{
			for (const FSkeletalMeshLODRenderData& LOD : RenderData->LODRenderData)
			{
				Text += (Text.IsEmpty() ? TEXT("") : TEXT("/")) + FString::FromInt(static_cast<int32>(LOD.StaticVertexBuffers.PositionVertexBuffer.GetNumVertices()));
			}
		}
		return Text.IsEmpty() ? FString(TEXT("none")) : Text;
	};
	USkeletalMesh* Candidates[] = { Body.BodyMesh.Get(), Body.MergedHeadAndBodyMesh.Get() };
	const TCHAR* Names[] = { TEXT("body mesh"), TEXT("merged head+body mesh") };
	OutLog.Reset();
	for (int32 Index = 0; Index < 2; ++Index)
	{
		const int32 LOD = Candidates[Index] ? FMantleCosmeticExport::FindReferenceLOD(Candidates[Index], Contract) : INDEX_NONE;
		OutLog += FString::Printf(TEXT("%s%s LOD vertices %s"), OutLog.IsEmpty() ? TEXT("") : TEXT("; "), Names[Index], *Counts(Candidates[Index]));
		if (LOD != INDEX_NONE)
		{
			OutLog += FString::Printf(TEXT(" (LOD %d has the reference topology)"), LOD);
			if (OutOrdering)
			{
				// How this body was put in the reference order: as built, or by the UV match with flipped diagonals.
				FMantleBodyLODTopology Topology;
				TArray<FVector2f> UVs;
				TArray<uint32> Indices;
				const FMantleBodyCorrespondence* Layout = FMantleBodyCorrespondence::Get();
				const FSkeletalMeshRenderData* RenderData = Candidates[Index]->GetResourceForRendering();
				FString How = TEXT("reference order");
				if (Layout && RenderData && UMantleBodyTopologyData::ReadLOD(Candidates[Index], LOD, Topology, nullptr, &UVs) && RenderData->LODRenderData.IsValidIndex(LOD))
				{
					RenderData->LODRenderData[LOD].MultiSizeIndexContainer.GetIndexBuffer(Indices);
					TArray<int32> Map;
					FString Error;
					if (Layout->Match(UVs, Indices, Map, Error)) { How = TEXT("exact UV/connectivity match"); }
					else if (Layout->MatchFlippedDiagonals(UVs, Indices, Map, How, Error)) { }
					else { How = Error; }
				}
				*OutOrdering = FString::Printf(TEXT("%s: %s LOD %d, %s"), *Body.Id, Names[Index], LOD, *How);
			}
			UE_LOG(LogMantleFitBodies, Display, TEXT("MANTLE_FIT_BODIES %s: %s"), *Body.Id, *OutLog);
			return Candidates[Index];
		}
	}
	OutLog += TEXT(". Why the body mesh does not match: ") + DescribeReferenceTopology(Body.BodyMesh.Get(), Contract);
	UE_LOG(LogMantleFitBodies, Warning, TEXT("MANTLE_FIT_BODIES %s: no LOD with the reference topology: %s"), *Body.Id, *OutLog);
	return nullptr;
}

bool FMantleFitBodies::MakeSet(UObject* Outfit, const EMantleFitSex Requested, const FMantleAvatarContract& Contract,
	TSharedPtr<FMantleFitBodySet>& OutSet, FString& OutError)
{
	using namespace MantleFitBodiesPrivate;
	OutSet.Reset();
	const TArray<FString> Suffixes = SanitizeBodyTypes(Contract.GarmentFitBodyTypes);
	if (Suffixes.IsEmpty())
	{
		OutError = TEXT("garment-fit.json bodyTypes names no standard body type");
		return false;
	}
	TSharedRef<FMantleFitBodySet> Set = MakeShared<FMantleFitBodySet>();
	// The two medium/normal bodies the sex is judged against; whichever sex wins is reused if its type is in the list.
	FMantleFitBody ProbeFemale, ProbeMale;
	if (Requested == EMantleFitSex::Auto)
	{
		TArray<FMantleOutfitSourceSize> Sizes;
		FString SizesError;
		TArray<float> FemaleCm, MaleCm;
		if (FMantleParametricOutfit::ListSizes(Outfit, Sizes, SizesError))
		{
			for (const EMantleFitSex Sex : { EMantleFitSex::Female, EMantleFitSex::Male })
			{
				FMantleFitBody& Probe = Sex == EMantleFitSex::Male ? ProbeMale : ProbeFemale;
				FString ProbeError;
				TArray<FVector3f> Reference;
				const bool bMade = Make(Sex == EMantleFitSex::Male ? TEXT("m_med_nrw") : TEXT("f_med_nrw"),
					Sex == EMantleFitSex::Male ? EMetaHumanBodyType::m_med_nrw : EMetaHumanBodyType::f_med_nrw, Probe, ProbeError);
				const bool bRead = bMade && BodyVertices(Probe, Reference);
				for (const FMantleOutfitSourceSize& Size : Sizes)
				{
					(Sex == EMantleFitSex::Male ? MaleCm : FemaleCm).Add(bRead ? SizeDistanceCm(Size, Reference) : -1.0f);
				}
			}
		}
		FString Why;
		Set->Sex = ChooseSex(FemaleCm, MaleCm, Contract.GarmentFitSexMarginCm, Why);
		Set->Status = FString::Printf(TEXT("Garment variants are fitted to Epic's standard %s bodies, chosen automatically: %s."),
			Set->Sex == EMantleFitSex::Male ? TEXT("male") : TEXT("female"), *Why);
	}
	else
	{
		Set->Sex = Requested;
		Set->Status = FString::Printf(TEXT("Garment variants are fitted to Epic's standard %s bodies, as chosen for this item."),
			Set->Sex == EMantleFitSex::Male ? TEXT("male") : TEXT("female"));
	}
	UE_LOG(LogMantleFitBodies, Display, TEXT("MANTLE_FIT_BODIES %s"), *Set->Status);

	const TArray<FString> Ids = VariantIds(Set->Sex, Suffixes);
	for (const FString& Id : Ids)
	{
		FMantleFitBody Body;
		FMantleFitBody* Probe = Set->Sex == EMantleFitSex::Male ? &ProbeMale : &ProbeFemale;
		if (Probe && Probe->Id == Id && Probe->Character.IsValid())
		{
			Body = MoveTemp(*Probe);
			*Probe = FMantleFitBody();
		}
		else
		{
			EMetaHumanBodyType Type = EMetaHumanBodyType::f_med_nrw;
			if (!ParseBodyType(Id, Type) || !Make(Id, Type, Body, OutError))
			{
				Release(ProbeFemale); Release(ProbeMale);
				return false;
			}
		}
		Set->Bodies.Add(MoveTemp(Body));
	}
	Release(ProbeFemale); Release(ProbeMale);
	OutSet = Set;
	return true;
}

bool FMantleFitBodies::ReadVertices(const USkeletalMesh* Mesh, int32 LODIndex, TArray<FVector3f>& OutPositions)
{
	OutPositions.Reset();
	if (!Mesh)
	{
		return false;
	}
	const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
	if (RenderData && RenderData->LODRenderData.IsValidIndex(LODIndex))
	{
		const FPositionVertexBuffer& Positions = RenderData->LODRenderData[LODIndex].StaticVertexBuffers.PositionVertexBuffer;
		const int32 Count = static_cast<int32>(Positions.GetNumVertices());
		if (Count > 0 && Positions.GetVertexData())
		{
			OutPositions.Reserve(Count);
			for (int32 Index = 0; Index < Count; ++Index)
			{
				OutPositions.Add(Positions.VertexPosition(static_cast<uint32>(Index)));
			}
			return true;
		}
	}
	if (LODIndex >= 0 && LODIndex < Mesh->GetLODNum() && Mesh->HasMeshDescription(LODIndex))
	{
		if (const FMeshDescription* Description = Mesh->GetMeshDescription(LODIndex))
		{
			FSkeletalMeshConstAttributes Attributes(*Description);
			TVertexAttributesConstRef<FVector3f> Positions = Attributes.GetVertexPositions();
			if (Positions.IsValid())
			{
				OutPositions.Reserve(Description->Vertices().Num());
				for (const FVertexID Vertex : Description->Vertices().GetElementIDs())
				{
					OutPositions.Add(Positions.Get(Vertex));
				}
				return !OutPositions.IsEmpty();
			}
		}
	}
	return false;
}

bool FMantleFitBodies::CompareNearest(const TArray<FVector3f>& Probe, const TArray<FVector3f>& Reference, float& OutAverageCm, float& OutMaxCm)
{
	OutAverageCm = 0.0f;
	OutMaxCm = 0.0f;
	if (Probe.IsEmpty() || Reference.IsEmpty())
	{
		return false;
	}
	const auto CellOf = [](const FVector3f& Point)
	{
		return FIntVector(FMath::FloorToInt32(Point.X), FMath::FloorToInt32(Point.Y), FMath::FloorToInt32(Point.Z));
	};
	TMap<FIntVector, TArray<int32>> Grid;
	for (int32 Index = 0; Index < Reference.Num(); ++Index)
	{
		Grid.FindOrAdd(CellOf(Reference[Index])).Add(Index);
	}
	constexpr int32 MaxRing = 12;
	double Sum = 0.0;
	for (const FVector3f& Point : Probe)
	{
		const FIntVector Cell = CellOf(Point);
		float Best = TNumericLimits<float>::Max();
		for (int32 Ring = 0; Ring <= MaxRing && Best > static_cast<float>(Ring - 1); ++Ring)
		{
			for (int32 X = -Ring; X <= Ring; ++X)
			{
				for (int32 Y = -Ring; Y <= Ring; ++Y)
				{
					for (int32 Z = -Ring; Z <= Ring; ++Z)
					{
						if (FMath::Max3(FMath::Abs(X), FMath::Abs(Y), FMath::Abs(Z)) != Ring)
						{
							continue;
						}
						if (const TArray<int32>* Found = Grid.Find(FIntVector(Cell.X + X, Cell.Y + Y, Cell.Z + Z)))
						{
							for (const int32 Index : *Found)
							{
								Best = FMath::Min(Best, FVector3f::Distance(Point, Reference[Index]));
							}
						}
					}
				}
			}
		}
		if (Best == TNumericLimits<float>::Max() || Best > static_cast<float>(MaxRing))
		{
			// Nothing close in the grid: look at every vertex (rare; a body far from the reference).
			for (const FVector3f& Candidate : Reference)
			{
				Best = FMath::Min(Best, FVector3f::Distance(Point, Candidate));
			}
		}
		Sum += Best;
		OutMaxCm = FMath::Max(OutMaxCm, Best);
	}
	OutAverageCm = static_cast<float>(Sum / Probe.Num());
	return true;
}
