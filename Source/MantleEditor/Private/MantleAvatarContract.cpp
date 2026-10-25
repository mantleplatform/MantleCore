// Copyright Joshua Eger. All Rights Reserved.
#include "MantleAvatarContract.h"
#include "MantleFitBodies.h"

#include "Dom/JsonObject.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantleAvatarContractPrivate
{
	bool ReadJsonFile(const FString& Path, TSharedPtr<FJsonObject>& OutObject, FString& OutError)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			OutError = FString::Printf(TEXT("could not read %s"), *Path);
			return false;
		}
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid())
		{
			OutError = FString::Printf(TEXT("%s is not valid JSON"), *Path);
			return false;
		}
		return true;
	}

	int32 IntField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int32 Default)
	{
		double Value = 0.0;
		return Object.IsValid() && Object->TryGetNumberField(Field, Value) ? static_cast<int32>(Value) : Default;
	}

	float FloatField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, float Default)
	{
		double Value = 0.0;
		return Object.IsValid() && Object->TryGetNumberField(Field, Value) ? static_cast<float>(Value) : Default;
	}

	TSharedPtr<FJsonObject> ObjectField(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject>* Found = nullptr;
		return Object.IsValid() && Object->TryGetObjectField(Field, Found) && Found ? *Found : TSharedPtr<FJsonObject>();
	}

	TArray<FString> StringArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		TArray<FString> Out;
		if (Object.IsValid())
		{
			Object->TryGetStringArrayField(Field, Out);
		}
		return Out;
	}

	FMantleContractClass ReadClass(const TSharedPtr<FJsonObject>& Limits)
	{
		FMantleContractClass Class;
		Class.MaxTriangles = IntField(Limits, TEXT("maxTriangles"), 0);
		Class.MaxSections = IntField(Limits, TEXT("maxSections"), 0);
		Class.MaxMaterials = IntField(Limits, TEXT("maxMaterials"), 0);
		Class.MaxInfluences = IntField(Limits, TEXT("maxInfluencesPerVertex"), 8);
		Class.Skeletons = StringArray(Limits, TEXT("skeletons"));
		const TSharedPtr<FJsonObject> Textures = ObjectField(Limits, TEXT("textures"));
		Class.AlbedoMax = IntField(Textures, TEXT("albedoMax"), 0);
		Class.NormalMax = IntField(Textures, TEXT("normalMax"), 0);
		Class.OrmMax = IntField(Textures, TEXT("ormMax"), 0);
		return Class;
	}

	bool ReadSkeleton(const FString& Path, const FString& Id, FMantleContractSkeleton& Out, FString& OutError)
	{
		TSharedPtr<FJsonObject> Root;
		if (!ReadJsonFile(Path, Root, OutError))
		{
			return false;
		}
		Out = FMantleContractSkeleton();
		Out.Id = Id;
		const TArray<TSharedPtr<FJsonValue>>* Bones = nullptr;
		if (!Root->TryGetArrayField(TEXT("bones"), Bones) || Bones->Num() == 0)
		{
			OutError = FString::Printf(TEXT("%s has no bones"), *Path);
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Value : *Bones)
		{
			const TSharedPtr<FJsonObject> Bone = Value.IsValid() ? Value->AsObject() : nullptr;
			FString Name;
			if (!Bone.IsValid() || !Bone->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
			{
				OutError = FString::Printf(TEXT("%s has a bone with no name"), *Path);
				return false;
			}
			const int32 Parent = IntField(Bone, TEXT("parent"), INDEX_NONE);
			if (Parent >= Out.Bones.Num() || (Parent < 0 && Out.Bones.Num() > 0))
			{
				OutError = FString::Printf(TEXT("%s: bone %s is not parent-first with one root"), *Path, *Name);
				return false;
			}
			Out.IndexOf.Add(FName(*Name), Out.Bones.Num());
			Out.Bones.Add(FName(*Name));
			Out.Parents.Add(Parent);
		}
		const TArray<TSharedPtr<FJsonValue>>* Sockets = nullptr;
		if (Root->TryGetArrayField(TEXT("sockets"), Sockets))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Sockets)
			{
				const TSharedPtr<FJsonObject> Socket = Value.IsValid() ? Value->AsObject() : nullptr;
				FString Name, Bone;
				if (Socket.IsValid() && Socket->TryGetStringField(TEXT("name"), Name) && Socket->TryGetStringField(TEXT("bone"), Bone))
				{
					Out.Sockets.Add(FName(*Name), FName(*Bone));
				}
			}
		}
		// The skeleton's own reference pose as chain directions. Absent on a fixture not measured yet (the UE5
		// Mannequin); a chain that is not two bone names and three numbers is left out.
		if (const TSharedPtr<FJsonObject> Pose = ObjectField(Root, TEXT("referencePose")))
		{
			const TArray<TSharedPtr<FJsonValue>>* Chains = nullptr;
			if (Pose->TryGetStringField(TEXT("name"), Out.ReferencePoseName) && Pose->TryGetArrayField(TEXT("chains"), Chains))
			{
				for (const TSharedPtr<FJsonValue>& Value : *Chains)
				{
					const TSharedPtr<FJsonObject> Chain = Value.IsValid() ? Value->AsObject() : nullptr;
					FMantleContractReferenceChain Entry;
					const TArray<TSharedPtr<FJsonValue>>* Direction = nullptr;
					if (!Chain.IsValid() || !Chain->TryGetStringField(TEXT("from"), Entry.From) || !Chain->TryGetStringField(TEXT("to"), Entry.To)
						|| !Chain->TryGetArrayField(TEXT("direction"), Direction) || Direction->Num() != 3)
					{
						continue;
					}
					for (int32 Axis = 0; Axis < 3; ++Axis)
					{
						Entry.Direction[Axis] = (*Direction)[Axis].IsValid() ? (*Direction)[Axis]->AsNumber() : 0.0;
					}
					Out.ReferenceChains.Add(MoveTemp(Entry));
				}
			}
			if (!Out.HasReferencePose())
			{
				Out.ReferencePoseName.Reset();
				Out.ReferenceChains.Reset();
			}
		}
		return true;
	}
}

int32 FMantleContractSkeleton::Find(FName Bone) const
{
	const int32* Index = IndexOf.Find(Bone);
	return Index ? *Index : INDEX_NONE;
}

FName FMantleContractSkeleton::ParentOf(FName Bone) const
{
	const int32 Index = Find(Bone);
	if (Index == INDEX_NONE || !Parents.IsValidIndex(Index) || Parents[Index] < 0)
	{
		return NAME_None;
	}
	return Bones[Parents[Index]];
}

FString FMantleAvatarContract::DefaultDirectory()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MantleCore"));
	if (!Plugin.IsValid())
	{
		return FString();
	}
	return FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("AvatarContract"));
}

bool FMantleAvatarContract::LoadFromDirectory(const FString& Directory, FMantleAvatarContract& Out, FString& OutError)
{
	using namespace MantleAvatarContractPrivate;

	Out = FMantleAvatarContract();

	TSharedPtr<FJsonObject> Limits;
	if (!ReadJsonFile(FPaths::Combine(Directory, TEXT("limits.json")), Limits, OutError))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Bounds = ObjectField(Limits, TEXT("avatarBounds"));
	Out.HeightMinCm = FloatField(Bounds, TEXT("heightMinCm"), 0.0f);
	Out.HeightMaxCm = FloatField(Bounds, TEXT("heightMaxCm"), 0.0f);
	Out.WidthMaxCm = FloatField(Bounds, TEXT("widthMaxCm"), 0.0f);
	Out.DepthMaxCm = FloatField(Bounds, TEXT("depthMaxCm"), 0.0f);

	// The size the game plays an avatar at, for the panel's "Plays at X cm".
	if (const TSharedPtr<FJsonObject> Scale = ObjectField(Limits, TEXT("characterScale")))
	{
		FMantleCharacterScaleRule Rule;
		Scale->TryGetNumberField(TEXT("driverStandingHeightCm"), Rule.DriverStandingHeightCm);
		Scale->TryGetNumberField(TEXT("minStandingHeightCm"), Rule.MinStandingHeightCm);
		Scale->TryGetNumberField(TEXT("maxStandingHeightCm"), Rule.MaxStandingHeightCm);
		if (Rule.IsValid())
		{
			Out.CharacterScale = Rule;
		}
	}

	// The standard body types garment variants are fitted to. Absent or unusable: the defaults in the header.
	// Plugin-only (Resources/GarmentFit): limits.json is a copy of the mantle repo's, so this list is not in it.
	TSharedPtr<FJsonObject> Fit;
	FString FitError;
	if (const FString FitPath = FPaths::Combine(Directory, TEXT(".."), TEXT("GarmentFit"), TEXT("garment-fit.json")); FPaths::FileExists(FitPath) && ReadJsonFile(FitPath, Fit, FitError) && Fit.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* TypeValues = nullptr;
		if (Fit->TryGetArrayField(TEXT("bodyTypes"), TypeValues))
		{
			TArray<FString> Types;
			for (const TSharedPtr<FJsonValue>& Value : *TypeValues)
			{
				FString Type;
				if (Value.IsValid() && Value->TryGetString(Type))
				{
					Types.Add(Type);
				}
			}
			Types = FMantleFitBodies::SanitizeBodyTypes(Types);
			if (!Types.IsEmpty())
			{
				Out.GarmentFitBodyTypes = MoveTemp(Types);
			}
		}
		double Margin = 0.0;
		if (Fit->TryGetNumberField(TEXT("sexMarginCm"), Margin) && Margin >= 0.0)
		{
			Out.GarmentFitSexMarginCm = static_cast<float>(Margin);
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* SlotValues = nullptr;
	if (!Limits->TryGetArrayField(TEXT("slots"), SlotValues))
	{
		OutError = TEXT("limits.json has no slots");
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *SlotValues)
	{
		const TSharedPtr<FJsonObject> Slot = Value.IsValid() ? Value->AsObject() : nullptr;
		if (!Slot.IsValid())
		{
			continue;
		}
		FMantleContractSlot& Entry = Out.Slots.AddDefaulted_GetRef();
		Slot->TryGetStringField(TEXT("id"), Entry.Id);
		Slot->TryGetStringField(TEXT("group"), Entry.Group);
		Slot->TryGetStringField(TEXT("itemClass"), Entry.ItemClass);
		Slot->TryGetStringField(TEXT("socket"), Entry.Socket);
		Slot->TryGetStringField(TEXT("socketBone"), Entry.SocketBone);
		Entry.Cap = IntField(Slot, TEXT("cap"), 1);
	}

	// Sockets an avatar may override besides the accessory sockets (hand_rSocket). Absent in an older copy: none.
	const TArray<TSharedPtr<FJsonValue>>* ToolSocketValues = nullptr;
	if (Limits->TryGetArrayField(TEXT("toolSockets"), ToolSocketValues))
	{
		for (const TSharedPtr<FJsonValue>& Value : *ToolSocketValues)
		{
			const TSharedPtr<FJsonObject> Tool = Value.IsValid() ? Value->AsObject() : nullptr;
			FMantleContractToolSocket Entry;
			if (!Tool.IsValid() || !Tool->TryGetStringField(TEXT("socket"), Entry.Socket) || !Tool->TryGetStringField(TEXT("socketBone"), Entry.SocketBone)
				|| Entry.Socket.IsEmpty() || Entry.SocketBone.IsEmpty())
			{
				OutError = TEXT("limits.json has a toolSockets entry without a socket and socketBone");
				return false;
			}
			Tool->TryGetBoolField(TEXT("unitScale"), Entry.bUnitScale);
			Out.ToolSockets.Add(MoveTemp(Entry));
		}
	}

	const TSharedPtr<FJsonObject> Content = ObjectField(Limits, TEXT("contentLimits"));
	if (!Content.IsValid())
	{
		OutError = TEXT("limits.json has no contentLimits");
		return false;
	}

	const TSharedPtr<FJsonObject> Body = ObjectField(Content, TEXT("avatar_body"));
	Out.AvatarBody = ReadClass(Body);
	Out.FeetToleranceCm = FloatField(Body, TEXT("feetAtOriginToleranceCm"), 10.0f);

	// The bones an avatar body must carry, per skeleton, and how close its reference pose must be to the
	// skeleton's own (cook.checkAvatarBodyShape). Absent in an older copy: no bone list, the cook's tolerances.
	if (const TSharedPtr<FJsonObject> Required = ObjectField(Body, TEXT("requiredBones")))
	{
		for (const auto& Pair : Required->Values) // keyed by UE::TSharedString in 5.8, not FString
		{
			TArray<FString> Names;
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (Pair.Value.IsValid() && Pair.Value->TryGetArray(Values))
			{
				for (const TSharedPtr<FJsonValue>& Name : *Values)
				{
					FString Text;
					if (Name.IsValid() && Name->TryGetString(Text) && !Text.IsEmpty())
					{
						Names.Add(Text);
					}
				}
			}
			Out.AvatarRequiredBones.Add(FString(Pair.Key.ToView()), MoveTemp(Names));
		}
	}
	if (const TSharedPtr<FJsonObject> PoseShape = ObjectField(Body, TEXT("referencePoseShape")))
	{
		double Number = 0.0;
		if (PoseShape->TryGetNumberField(TEXT("chainToleranceDegrees"), Number) && Number > 0.0 && Number < 90.0)
		{
			Out.ChainToleranceDegrees = Number;
		}
		if (PoseShape->TryGetNumberField(TEXT("boneScaleTolerance"), Number) && Number > 0.0 && Number < 1.0)
		{
			Out.BoneScaleTolerance = Number;
		}
	}

	const TSharedPtr<FJsonObject> Face = ObjectField(Content, TEXT("avatar_face"));
	Out.AvatarFace = ReadClass(Face);
	FString JawDefault;
	if (Face.IsValid() && Face->TryGetStringField(TEXT("jawBoneDefault"), JawDefault) && !JawDefault.IsEmpty())
	{
		Out.FaceJawBoneDefault = FName(*JawDefault);
	}

	const TSharedPtr<FJsonObject> Rigid = ObjectField(Content, TEXT("rigid_accessory"));
	Out.RigidAccessory = ReadClass(Rigid);
	if (const TSharedPtr<FJsonObject> PerSlot = ObjectField(Rigid, TEXT("boundsPerSlot")))
	{
		for (const auto& Pair : PerSlot->Values) // keyed by UE::TSharedString in 5.8, not FString
		{
			const TSharedPtr<FJsonObject> Entry = Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
			FMantleContractAccessoryBounds SlotBounds;
			SlotBounds.MaxDimensionCm = FloatField(Entry, TEXT("maxDimensionCm"), 0.0f);
			SlotBounds.MaxReachCm = FloatField(Entry, TEXT("maxReachCm"), 0.0f);
			Out.AccessoryBounds.Add(FString(Pair.Key.ToView()), SlotBounds);
		}
	}

	const TSharedPtr<FJsonObject> Garment = ObjectField(Content, TEXT("skinned_garment"));
	Out.SkinnedGarment = ReadClass(Garment);
	const TSharedPtr<FJsonObject> Tolerance = ObjectField(Garment, TEXT("bindPoseTolerance"));
	Out.BindToleranceCm = FloatField(Tolerance, TEXT("translationCm"), 0.5f);
	Out.BindToleranceDegrees = FloatField(Tolerance, TEXT("rotationDegrees"), 1.0f);
	const TSharedPtr<FJsonObject> Coverage = ObjectField(Garment, TEXT("coverage"));
	Out.CoverageThresholdCm = FloatField(Coverage, TEXT("uploadThresholdCm"), 3.0f);
	for (const FString& Knee : StringArray(ObjectField(Garment, TEXT("longGarment")), TEXT("kneeBones")))
	{
		Out.KneeBones.Add(FName(*Knee));
	}
	const TSharedPtr<FJsonObject> Hair = ObjectField(Garment, TEXT("hair"));
	Out.HairSlots = StringArray(Hair, TEXT("slots"));
	const TSharedPtr<FJsonObject> HairCaps = ObjectField(Hair, TEXT("maxTrianglesPerSlot"));
	if (!HairCaps || HairCaps->Values.Num() != Out.HairSlots.Num())
	{
		OutError = TEXT("hair triangle caps must name every hair slot exactly once");
		return false;
	}
	for (const FString& HairSlot : Out.HairSlots)
	{
		double Cap = 0.0;
		if (!HairCaps->TryGetNumberField(HairSlot, Cap) || Cap <= 0.0 || Cap > 500000.0 || Cap != FMath::FloorToDouble(Cap))
		{
			OutError = TEXT("invalid hair triangle cap for ") + HairSlot;
			return false;
		}
		Out.HairMaxTrianglesPerSlot.Add(HairSlot, static_cast<int32>(Cap));
	}
	Out.HairMaxTextureSize = IntField(Hair, TEXT("maxTextureSize"), 2048);

	const TSharedPtr<FJsonObject> Shape = ObjectField(Content, TEXT("body_shape"));
	Out.WarpPointCount = IntField(Shape, TEXT("warpPointCount"), 750);

	// The LODs the plugin generates, as shares of LOD0's triangles.
	const TArray<TSharedPtr<FJsonValue>>* Fractions = nullptr;
	const TSharedPtr<FJsonObject> Lods = ObjectField(Limits, TEXT("lods"));
	if (Lods.IsValid() && Lods->TryGetArrayField(TEXT("targetFractions"), Fractions) && Fractions->Num() > 0)
	{
		Out.LODTargetFractions.Reset();
		for (const TSharedPtr<FJsonValue>& Fraction : *Fractions)
		{
			Out.LODTargetFractions.Add(Fraction.IsValid() ? static_cast<float>(Fraction->AsNumber()) : 0.0f);
		}
	}

	const TSharedPtr<FJsonObject> Emote = ObjectField(Content, TEXT("emote"));
	Out.EmoteFrameRate = FloatField(Emote, TEXT("frameRate"), 30.0f);
	Out.EmoteMaxSeconds = FloatField(Emote, TEXT("maxDurationSeconds"), 10.0f);
	Out.EmoteMaxRootCm = FloatField(Emote, TEXT("maxRootTranslationCm"), 50.0f);
	Out.EmoteMaxEvents = IntField(Emote, TEXT("maxEvents"), 64);
	Out.EmoteMaxBytes = IntField(Emote, TEXT("maxCookedBytes"), 524288);
	Out.EmoteEvents = StringArray(Emote, TEXT("events"));
	for (const FString& Bone : StringArray(Emote, TEXT("requiredBones")))
	{
		Out.EmoteRequiredBones.Add(FName(*Bone));
	}
	const TArray<FString> EmoteSkeletons = StringArray(Emote, TEXT("skeletons"));
	if (EmoteSkeletons.Num() > 0)
	{
		Out.EmoteSkeleton = EmoteSkeletons[0];
	}

	const TSharedPtr<FJsonObject> Thumbnail = ObjectField(Content, TEXT("thumbnail"));
	Out.ThumbnailSize = IntField(Thumbnail, TEXT("width"), 512);

	const TSharedPtr<FJsonObject> AnyUpload = ObjectField(Content, TEXT("any_upload"));
	double MaxBytes = 0.0;
	if (AnyUpload.IsValid() && AnyUpload->TryGetNumberField(TEXT("maxBytes"), MaxBytes))
	{
		Out.MaxUploadBytes = static_cast<int64>(MaxBytes);
	}
	Out.MaxTextureSize = IntField(AnyUpload, TEXT("maxTextureSize"), 2048);

	const TSharedPtr<FJsonObject> PlaceMesh = ObjectField(ObjectField(Limits, TEXT("placeAssets")), TEXT("staticMesh"));
	Out.PlaceMeshMaxTriangles = IntField(PlaceMesh, TEXT("maxTriangles"), 500000);
	Out.PlaceMeshMaxVertices = IntField(PlaceMesh, TEXT("maxVertices"), 1500000);
	Out.PlaceMeshMaxSections = IntField(PlaceMesh, TEXT("maxSections"), 8);

	const TSharedPtr<FJsonObject> PlaceSound = ObjectField(ObjectField(Limits, TEXT("placeAssets")), TEXT("sound"));
	Out.PlaceSoundMinSampleRate = IntField(PlaceSound, TEXT("minSampleRate"), 22050);
	Out.PlaceSoundMaxSampleRate = IntField(PlaceSound, TEXT("maxSampleRate"), 48000);
	double SoundNumber = 0.0;
	if (PlaceSound.IsValid() && PlaceSound->TryGetNumberField(TEXT("maxDurationSeconds"), SoundNumber))
	{
		Out.PlaceSoundMaxSeconds = SoundNumber;
	}
	if (PlaceSound.IsValid() && PlaceSound->TryGetNumberField(TEXT("maxSourceBytes"), SoundNumber))
	{
		Out.PlaceSoundMaxSourceBytes = static_cast<int64>(SoundNumber);
	}
	if (PlaceSound.IsValid() && PlaceSound->TryGetNumberField(TEXT("maxPlaceDecodedBytes"), SoundNumber))
	{
		Out.PlaceSoundMaxDecodedBytes = static_cast<int64>(SoundNumber);
	}

	// The authored LODs and simple collision an uploaded place mesh may carry.
	const TSharedPtr<FJsonObject> PlaceLods = ObjectField(PlaceMesh, TEXT("lods"));
	Out.PlaceMeshMaxLODs = IntField(PlaceLods, TEXT("maxLods"), 3);
	const TArray<TSharedPtr<FJsonValue>>* LodTriangles = nullptr;
	if (PlaceLods.IsValid() && PlaceLods->TryGetArrayField(TEXT("maxTriangles"), LodTriangles) && LodTriangles->Num() > 0)
	{
		Out.PlaceMeshLODMaxTriangles.Reset();
		for (const TSharedPtr<FJsonValue>& Triangles : *LodTriangles)
		{
			Out.PlaceMeshLODMaxTriangles.Add(Triangles.IsValid() ? static_cast<int32>(Triangles->AsNumber()) : 0);
		}
	}
	const TSharedPtr<FJsonObject> PlaceCollision = ObjectField(PlaceMesh, TEXT("collision"));
	Out.PlaceMeshMaxCollisionPrimitives = IntField(PlaceCollision, TEXT("maxPrimitives"), 64);
	Out.PlaceMeshMaxConvexHulls = IntField(PlaceCollision, TEXT("maxConvexHulls"), 64);
	Out.PlaceMeshMaxHullVertices = IntField(PlaceCollision, TEXT("maxHullVertices"), 255);
	Out.PlaceMeshMaxTotalHullVertices = IntField(PlaceCollision, TEXT("maxTotalHullVertices"), 8192);

	// A place skeletal mesh on its own skeleton and a clip for it.
	const TSharedPtr<FJsonObject> PlaceSkeletal = ObjectField(ObjectField(Limits, TEXT("placeAssets")), TEXT("skeletalMesh"));
	Out.PlaceSkeletalMaxTriangles = IntField(PlaceSkeletal, TEXT("maxTriangles"), 100000);
	Out.PlaceSkeletalMaxSections = IntField(PlaceSkeletal, TEXT("maxSections"), 8);
	Out.PlaceSkeletalMaxBones = IntField(PlaceSkeletal, TEXT("maxBones"), 256);
	const TSharedPtr<FJsonObject> PlaceClip = ObjectField(ObjectField(Limits, TEXT("placeAssets")), TEXT("animClip"));
	double ClipNumber = 0.0;
	if (PlaceClip.IsValid() && PlaceClip->TryGetNumberField(TEXT("maxDurationSeconds"), ClipNumber))
	{
		Out.PlaceClipMaxSeconds = ClipNumber;
	}
	if (PlaceClip.IsValid() && PlaceClip->TryGetNumberField(TEXT("maxFrameRate"), ClipNumber))
	{
		Out.PlaceClipMaxFrameRate = ClipNumber;
	}
	if (PlaceClip.IsValid() && PlaceClip->TryGetNumberField(TEXT("maxCookedBytes"), ClipNumber))
	{
		Out.PlaceClipMaxCookedBytes = static_cast<int64>(ClipNumber);
	}

	TSharedPtr<FJsonObject> Topology;
	if (!ReadJsonFile(FPaths::Combine(Directory, TEXT("body-topology-v1.json")), Topology, OutError))
	{
		return false;
	}
	Out.TopologyVertexCount = IntField(Topology, TEXT("vertexCount"), 0);
	FString UVHash;
	if (!Topology->TryGetStringField(TEXT("uvHash"), UVHash) || UVHash.Len() != 8 || Out.TopologyVertexCount <= 0)
	{
		OutError = TEXT("body-topology-v1.json has no vertexCount or 8-digit uvHash");
		return false;
	}
	Out.TopologyUVHash = static_cast<uint32>(FParse::HexNumber(*UVHash));

	const TArray<TSharedPtr<FJsonValue>>* SkeletonEntries = nullptr;
	if (Limits->TryGetArrayField(TEXT("skeletons"), SkeletonEntries))
	{
		for (const TSharedPtr<FJsonValue>& Value : *SkeletonEntries)
		{
			const TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
			FString Id, Fixture;
			if (!Entry.IsValid() || !Entry->TryGetStringField(TEXT("id"), Id) || !Entry->TryGetStringField(TEXT("fixture"), Fixture))
			{
				continue;
			}
			const FString Path = FPaths::Combine(Directory, Fixture);
			if (!FPaths::FileExists(Path))
			{
				// A fixture the contract names but an older copy lacks: the worker skips it too.
				continue;
			}
			FMantleContractSkeleton Skeleton;
			if (!ReadSkeleton(Path, Id, Skeleton, OutError))
			{
				return false;
			}
			Out.Skeletons.Add(Id, MoveTemp(Skeleton));
		}
	}
	if (!Out.Skeletons.Contains(TEXT("metahuman")))
	{
		OutError = TEXT("the metahuman skeleton fixture is missing");
		return false;
	}

	// The contract copy remains as an explicit drift check. No retired default body or catalog
	// seed may be supplied by an older or hand-edited plugin resource.
	const FString BasesPath = FPaths::Combine(Directory, TEXT("base-avatars.json"));
	TSharedPtr<FJsonObject> Bases;
	if (!ReadJsonFile(BasesPath, Bases, OutError))
	{
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* BaseEntries = nullptr;
	if (!Bases->TryGetArrayField(TEXT("avatars"), BaseEntries) || !BaseEntries || BaseEntries->Num() != 0 || Bases->HasField(TEXT("default")))
	{
		OutError = TEXT("base-avatars.json must have an empty avatars list and no default body");
		return false;
	}
	return true;
}

const FMantleAvatarContract* FMantleAvatarContract::Get(FString* OutError)
{
	static TUniquePtr<FMantleAvatarContract> Loaded;
	static FString LoadError;
	if (!Loaded.IsValid() && LoadError.IsEmpty())
	{
		TUniquePtr<FMantleAvatarContract> Contract = MakeUnique<FMantleAvatarContract>();
		const FString Directory = DefaultDirectory();
		if (Directory.IsEmpty())
		{
			LoadError = TEXT("the MantleCore plugin was not found");
		}
		else if (LoadFromDirectory(Directory, *Contract, LoadError))
		{
			Loaded = MoveTemp(Contract);
		}
		if (!Loaded.IsValid() && LoadError.IsEmpty())
		{
			LoadError = TEXT("the avatar contract could not be read");
		}
	}
	if (!Loaded.IsValid() && OutError)
	{
		*OutError = FString::Printf(TEXT("The Mantle plugin's avatar contract could not be read (%s). Reinstall the plugin."), *LoadError);
	}
	return Loaded.Get();
}

const FMantleContractSlot* FMantleAvatarContract::FindSlot(const FString& SlotId) const
{
	return Slots.FindByPredicate([&SlotId](const FMantleContractSlot& Slot) { return Slot.Id == SlotId; });
}

int32 FMantleAvatarContract::SkinnedMaxTriangles(const FString& Slot) const
{
	const int32* Cap = HairMaxTrianglesPerSlot.Find(Slot);
	return Cap ? *Cap : SkinnedGarment.MaxTriangles;
}

TArray<FString> FMantleAvatarContract::AccessorySockets() const
{
	TArray<FString> Out;
	for (const FMantleContractSlot& Slot : Slots)
	{
		if (!Slot.Socket.IsEmpty())
		{
			Out.AddUnique(Slot.Socket);
		}
	}
	return Out;
}

const FMantleContractToolSocket* FMantleAvatarContract::FindToolSocket(const FString& Socket) const
{
	return ToolSockets.FindByPredicate([&Socket](const FMantleContractToolSocket& Tool) { return Tool.Socket == Socket; });
}

TArray<FString> FMantleAvatarContract::SlotsOfClass(const FString& ItemClass) const
{
	TArray<FString> Out;
	for (const FMantleContractSlot& Slot : Slots)
	{
		if (Slot.ItemClass == ItemClass)
		{
			Out.Add(Slot.Id);
		}
	}
	return Out;
}
