// Copyright Joshua Eger. All Rights Reserved.
#include "MantlePreviewExport.h"
#include "MantlePlaceSecrets.h"
#include "MantleAssetStore.h"
#include "MantlePlaceAssetConverter.h"
#include "MantlePlaceExporter.h"
#include "MantlePreviewShared.h"
#include "MantleCoverageSurvey.h"
#include "MantlePlaceValidator.h"
#include "MantleClientFeatures.h"
#include "MantlePreparedPlaceAssets.h"
#include "MantleCosmeticPackage.h"
#include "MantlePreviewAvatarExport.h"
#include "MantlePreviewLaunch.h"
#include "MantlePublishCaches.h"
#include "MantleRuntime.h"
#include "MantleQuickJSRuntime.h"
#include "MantlePlaceLimits.h"
#include "MantleScriptPackages.h"
#include "MantleScriptError.h"

#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Materials/Material.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"

// Named, not anonymous: MantleEditor builds as a unity build.
namespace MantlePreviewExportPrivate
{
	/** One cook of the local tool may not run longer than this (a large mesh or heightfield is the slow case). */
	constexpr double CookTimeoutSeconds = 900.0;

	/** The bundle limits of the manifest contract (MantlePreviewLaunch). */
	constexpr int32 MaxAssets = 4096;
	constexpr int64 MaxAssetFileBytes = 128LL * 1024 * 1024;
	constexpr int64 MaxAssetTotalBytes = 2LL * 1024 * 1024 * 1024;

	/** One asset entry of the manifest. */
	struct FBundleAsset
	{
		FString Id;
		FString Kind;
		/** Bundle-relative: Assets/<64 hex>. */
		FString File;
		/** "blake3:" + 64 lowercase hex. */
		FString CookedHash;
		int64 CookedBytes = 0;
		FString Role;
		/** Materials: texture info key (baseColor, layerBaseColor, ...) to the texture id. */
		TMap<FString, FString> Textures;
		/** The cook's info object for this asset (mantle-cook's result.info; cook.Result.Info): what the vehicle checks read. May be null. */
		TSharedPtr<FJsonObject> Info;
	};

	/** One script or UI entry of the manifest. */
	struct FBundleScript
	{
		FString Path;
		FString Side;
		FString File;
		FString Text;
		/** TypeScript only: the compiler's v3 source map, written to Maps/<Path>.map. Empty for every other script. */
		FString MapFile;
		FString MapText;
	};

	struct FBundleUIDocument
	{
		FString Path;
		FString File;
	};

	struct FExportState
	{
		FString CookTool;

		/** mantle-cook's --allow-features value (#628): the target client's features. Empty when it reads none. */
		FString AllowFeatures;

		/** The generation folder, once this export created it: the only folder a failure deletes. */
		FString BundleDir;
		FString WorkDir;

		TArray<FBundleAsset> Assets;
		TMap<FString, int32> AssetIndexById;
		TSet<FString> WrittenFiles;
		int64 TotalAssetBytes = 0;
		int32 CookCount = 0;

		/** Run's progress dialog (null when none) and whether the creator cancelled it. */
		FScopedSlowTask* Progress = nullptr;
		bool bCancelled = false;
	};

	/** The progress dialog's share of the whole export, in units of a total of 100. The cook stages split the rest evenly. */
	constexpr float ProgressValidate = 5.f;
	constexpr float ProgressConvert = 15.f;
	/** Every cook stage together: what the other steps leave of the 100, split evenly by however many stages there are. The stages must not take more than this between them, or the progress dialog overflows its total. */
	constexpr float ProgressCookStages = 64.f;
	constexpr float ProgressScripts = 10.f;
	constexpr float ProgressManifest = 6.f;
	static_assert(ProgressValidate + ProgressConvert + ProgressCookStages + ProgressScripts + ProgressManifest <= 100.f, "the export's steps fit the dialog's 100");

	/** True once the creator pressed Cancel (and remembered in State, so Run reports it). Also keeps the dialog responsive. */
	bool PreviewCancelled(FExportState& State)
	{
		if (!State.bCancelled && State.Progress && State.Progress->ShouldCancel())
		{
			State.bCancelled = true;
		}
		return State.bCancelled;
	}

	/** Starts the next progress frame (Work units, Message) and returns true when the creator has cancelled. */
	bool PreviewStageCancelled(FExportState& State, float Work, const FString& Message)
	{
		if (State.Progress)
		{
			State.Progress->EnterProgressFrame(Work, FText::FromString(Message));
		}
		return PreviewCancelled(State);
	}

	/** Deletes its files when it goes out of scope. */
	struct FTempFiles
	{
		TArray<FString> Files;

		~FTempFiles()
		{
			for (const FString& File : Files)
			{
				IFileManager::Get().Delete(*File, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
			}
		}
	};

	/** One line for a log a harness greps: no line breaks, no runs of spaces. */
	FString PreviewOneLine(const FString& Text)
	{
		FString Line = Text;
		Line.ReplaceInline(TEXT("\r"), TEXT(" "));
		Line.ReplaceInline(TEXT("\n"), TEXT(" "));
		Line.ReplaceInline(TEXT("\t"), TEXT(" "));
		while (Line.ReplaceInline(TEXT("  "), TEXT(" ")) > 0)
		{
		}
		return Line.TrimStartAndEnd();
	}

	/** An argument value a creator may have quoted ("C:\some folder\mantle-cook.exe"). */
	FString PreviewUnquoted(const FString& Value)
	{
		FString Text = Value.TrimStartAndEnd();
		if (Text.Len() >= 2 && Text.StartsWith(TEXT("\"")) && Text.EndsWith(TEXT("\"")))
		{
			Text = Text.Mid(1, Text.Len() - 2);
		}
		return Text;
	}

	TSharedPtr<FJsonObject> PreviewReadJsonFile(const FString& File)
	{
		FString Text;
		TSharedPtr<FJsonObject> Object;
		if (!FFileHelper::LoadFileToString(Text, *File)
			|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object) || !Object.IsValid())
		{
			return nullptr;
		}
		return Object;
	}

	bool PreviewWriteText(const FString& File, const FString& Text)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
		return FFileHelper::SaveStringToFile(Text, *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	bool PreviewWriteBytes(const FString& File, const TArray<uint8>& Bytes)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
		return FFileHelper::SaveArrayToFile(Bytes, *File);
	}

	/** The cookOptions object the upload carries: only the fields it sets. Empty when it sets none. */
	FString PreviewCookOptionsJson(const FMantleAssetUploadOptions& Options)
	{
		if (Options.Part.IsEmpty() && Options.Role.IsEmpty() && Options.Source.IsEmpty())
		{
			return FString();
		}
		FString Json;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
		Writer->WriteObjectStart();
		if (!Options.Part.IsEmpty())
		{
			Writer->WriteValue(TEXT("part"), Options.Part);
		}
		if (!Options.Role.IsEmpty())
		{
			Writer->WriteValue(TEXT("role"), Options.Role);
		}
		if (!Options.Source.IsEmpty())
		{
			Writer->WriteValue(TEXT("source"), Options.Source);
		}
		Writer->WriteObjectEnd();
		Writer->Close();
		return Json;
	}

	/** This editor process's session folder: one guid, so successive exports are its generations. */
	const FString& PreviewProcessSession()
	{
		static const FString Session = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		return Session;
	}

	const FBundleAsset* PreviewFindAsset(const FExportState& State, const FString& Id)
	{
		const int32* Index = State.AssetIndexById.Find(Id);
		return Index ? &State.Assets[*Index] : nullptr;
	}

	/** The upload role (role=) a texture named under a material's info key must have (backend cook.MaterialTextureRole); null for a key it does not know. */
	const TCHAR* PreviewWantedTextureRole(const FString& InfoKey)
	{
		auto Is = [&InfoKey](const TCHAR* Expected) { return InfoKey.Equals(Expected, ESearchCase::CaseSensitive); };
		if (Is(TEXT("baseColor")) || Is(TEXT("emissive")) || Is(TEXT("layerBaseColor")))
		{
			return TEXT("color");
		}
		if (Is(TEXT("normal")) || Is(TEXT("layerNormal")) || Is(TEXT("waterNormal2")))
		{
			return TEXT("normal");
		}
		if (Is(TEXT("orm")) || Is(TEXT("layerOrm")))
		{
			return TEXT("data");
		}
		return nullptr;
	}

	/** A layer blend's document slot as cook_info names it (backend cook.MaterialLayerInfoKeys). */
	FString PreviewLayerInfoKey(const FString& Slot)
	{
		if (Slot.Equals(TEXT("baseColor"), ESearchCase::CaseSensitive)) return TEXT("layerBaseColor");
		if (Slot.Equals(TEXT("normal"), ESearchCase::CaseSensitive)) return TEXT("layerNormal");
		if (Slot.Equals(TEXT("orm"), ESearchCase::CaseSensitive)) return TEXT("layerOrm");
		return TEXT("layer") + Slot;
	}

	/**
	 * The backend worker's check of a material's textures (cook.checkMaterialTextures), done against the
	 * bundle instead of its database: every uploaded texture the material names is in the bundle, is a
	 * texture, and has the role its slot wants (a texture uploaded with no role serves any slot).
	 * OutTextures gets the info-key to id map the manifest lists.
	 */
	bool PreviewCheckMaterialTextures(const FExportState& State, const FString& Name, const TArray<uint8>& Bytes,
		TMap<FString, FString>& OutTextures, FString& OutError)
	{
		OutTextures.Reset();
		const FUTF8ToTCHAR Utf8(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		TSharedPtr<FJsonObject> Document;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(Utf8.Length(), Utf8.Get())), Document) || !Document.IsValid())
		{
			OutError = FString::Printf(TEXT("Material '%s' was prepared as something that is not a JSON document."), *Name);
			return false;
		}

		auto Collect = [&OutTextures](const TSharedPtr<FJsonObject>& Textures, bool bLayer)
		{
			if (!Textures.IsValid())
			{
				return;
			}
			for (const auto& Pair : Textures->Values)
			{
				FString Id;
				if (!Pair.Value.IsValid() || !Pair.Value->TryGetString(Id) || !FMantlePreparedPlaceAssets::IsUploadedId(Id))
				{
					continue;
				}
				const FString Slot(Pair.Key.ToView());
				OutTextures.Add(bLayer ? PreviewLayerInfoKey(Slot) : Slot, Id);
			}
		};
		const TSharedPtr<FJsonObject>* BaseTextures = nullptr;
		if (Document->TryGetObjectField(TEXT("textures"), BaseTextures) && BaseTextures)
		{
			Collect(*BaseTextures, false);
		}
		const TSharedPtr<FJsonObject>* Layer = nullptr;
		if (Document->TryGetObjectField(TEXT("layerBlend"), Layer) && Layer && Layer->IsValid())
		{
			const TSharedPtr<FJsonObject>* LayerTextures = nullptr;
			if ((*Layer)->TryGetObjectField(TEXT("textures"), LayerTextures) && LayerTextures)
			{
				Collect(*LayerTextures, true);
			}
		}
		// #628: water's second normal map, in cook_info as waterNormal2 (backend cook.MaterialWaterNormal2InfoKey).
		const TSharedPtr<FJsonObject>* Water = nullptr;
		if (Document->TryGetObjectField(TEXT("water"), Water) && Water && Water->IsValid())
		{
			const TSharedPtr<FJsonObject>* Normal2 = nullptr;
			FString Id;
			if ((*Water)->TryGetObjectField(TEXT("normal2"), Normal2) && Normal2 && Normal2->IsValid()
				&& (*Normal2)->TryGetStringField(TEXT("texture"), Id) && FMantlePreparedPlaceAssets::IsUploadedId(Id))
			{
				OutTextures.Add(TEXT("waterNormal2"), Id);
			}
		}

		TArray<FString> Slots;
		OutTextures.GetKeys(Slots);
		Slots.Sort();
		for (const FString& Slot : Slots)
		{
			const FString& Id = OutTextures.FindChecked(Slot);
			const FBundleAsset* Texture = PreviewFindAsset(State, Id);
			if (!Texture)
			{
				OutError = FString::Printf(TEXT("Material '%s' names texture %s for its %s map, but that texture is not in the preview bundle."), *Name, *Id, *Slot);
				return false;
			}
			if (!Texture->Kind.Equals(TEXT("texture"), ESearchCase::CaseSensitive))
			{
				OutError = FString::Printf(TEXT("Material '%s' names %s for its %s map, which is a %s asset, not a texture."), *Name, *Id, *Slot, *Texture->Kind);
				return false;
			}
			const TCHAR* Wanted = PreviewWantedTextureRole(Slot);
			if (Wanted && !Texture->Role.IsEmpty() && !Texture->Role.Equals(Wanted, ESearchCase::CaseSensitive))
			{
				OutError = FString::Printf(TEXT("Material '%s' names %s for its %s map, which was prepared as a %s texture; that map needs a %s texture."),
					*Name, *Id, *Slot, *Texture->Role, Wanted);
				return false;
			}
		}
		return true;
	}

	/**
	 * limits.json placeAssets.livery.alphaRequired, the flag the worker reads from its contract
	 * (checkVehicleMaterials). False when the file cannot be read, as the worker's own fallback is.
	 */
	bool PreviewLiveryAlphaRequired()
	{
		static const bool bRequired = []
		{
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MantleCore"));
			if (!Plugin.IsValid())
			{
				return false;
			}
			const TSharedPtr<FJsonObject> Limits = PreviewReadJsonFile(Plugin->GetBaseDir() / TEXT("Resources/AvatarContract/limits.json"));
			const TSharedPtr<FJsonObject>* PlaceAssets = nullptr;
			const TSharedPtr<FJsonObject>* Livery = nullptr;
			bool bAlpha = false;
			return Limits.IsValid() && Limits->TryGetObjectField(TEXT("placeAssets"), PlaceAssets) && PlaceAssets
				&& (*PlaceAssets)->TryGetObjectField(TEXT("livery"), Livery) && Livery
				&& (*Livery)->TryGetBoolField(TEXT("alphaRequired"), bAlpha) && bAlpha;
		}();
		return bRequired;
	}

	/**
	 * The worker's check of a vehicle (cook.checkVehicleMaterials), against the bundle: the uploaded
	 * materials, livery textures, part meshes and horn sounds its cooked info names are in it, of the
	 * right kind (a livery with no role or colour, and with alpha when the contract requires it), and
	 * each part names no more materials than its mesh's cooked info says it has slots, and paints
	 * only slots it has.
	 */
	bool PreviewCheckVehicleNames(const FExportState& State, const FString& Name, const TSharedPtr<FJsonObject>& Info, FString& OutError)
	{
		if (!Info.IsValid())
		{
			return true;
		}
		struct FGroup
		{
			const TCHAR* Field;
			const TCHAR* Kind;
			const TCHAR* What;
		};
		const FGroup Groups[] = {
			{ TEXT("materials"), TEXT("material"), TEXT("material") },
			{ TEXT("liveries"), TEXT("texture"), TEXT("livery") },
			{ TEXT("meshes"), TEXT("mesh"), TEXT("part mesh") },
			{ TEXT("sounds"), TEXT("sound"), TEXT("horn sound") },
		};
		for (const FGroup& Group : Groups)
		{
			const TArray<TSharedPtr<FJsonValue>>* Ids = nullptr;
			if (!Info->TryGetArrayField(Group.Field, Ids) || !Ids)
			{
				continue;
			}
			for (const TSharedPtr<FJsonValue>& Value : *Ids)
			{
				FString Id;
				// Library ids are Mantle's own, resolved by the client; only this level's uploads are in the bundle.
				if (!Value.IsValid() || !Value->TryGetString(Id) || !FMantlePreparedPlaceAssets::IsUploadedId(Id))
				{
					continue;
				}
				const FBundleAsset* Named = PreviewFindAsset(State, Id);
				if (!Named)
				{
					OutError = FString::Printf(TEXT("Vehicle '%s' names its %s %s, which is not in the preview bundle."), *Name, Group.What, *Id);
					return false;
				}
				if (!Named->Kind.Equals(Group.Kind, ESearchCase::CaseSensitive))
				{
					OutError = FString::Printf(TEXT("Vehicle '%s' names its %s %s, which is a %s asset, not a %s."), *Name, Group.What, *Id, *Named->Kind, Group.Kind);
					return false;
				}
				if (Named->Kind.Equals(TEXT("texture"), ESearchCase::CaseSensitive) && !Named->Role.IsEmpty() && !Named->Role.Equals(TEXT("color"), ESearchCase::CaseSensitive))
				{
					OutError = FString::Printf(TEXT("Vehicle '%s' names livery %s, which was prepared as a %s texture; a livery is a colour texture."), *Name, *Id, *Named->Role);
					return false;
				}
				bool bAlpha = false;
				if (Named->Kind.Equals(TEXT("texture"), ESearchCase::CaseSensitive) && Named->Info.IsValid() && PreviewLiveryAlphaRequired()
					&& (!Named->Info->TryGetBoolField(TEXT("alpha"), bAlpha) || !bAlpha))
				{
					OutError = FString::Printf(TEXT("Vehicle '%s' names livery %s, which has no alpha; a livery's alpha is where the paint shows through."), *Name, *Id);
					return false;
				}
			}
		}

		// Each part's material count and paint slots against the slots of the mesh it draws (the mesh's cooked
		// sections, as the worker reads them from the mesh's cook_info). A library mesh has no info here.
		const TArray<TSharedPtr<FJsonValue>>* PartSlots = nullptr;
		if (Info->TryGetArrayField(TEXT("partSlots"), PartSlots) && PartSlots)
		{
			for (int32 PartIndex = 0; PartIndex < PartSlots->Num(); ++PartIndex)
			{
				const TSharedPtr<FJsonObject>* Part = nullptr;
				FString MeshId;
				if (!(*PartSlots)[PartIndex].IsValid() || !(*PartSlots)[PartIndex]->TryGetObject(Part) || !Part || !(*Part)->TryGetStringField(TEXT("mesh"), MeshId)
					|| !FMantlePreparedPlaceAssets::IsUploadedId(MeshId))
				{
					continue;
				}
				const FBundleAsset* Mesh = PreviewFindAsset(State, MeshId);
				FString Format;
				if (!Mesh || !Mesh->Kind.Equals(TEXT("mesh"), ESearchCase::CaseSensitive) || !Mesh->Info.IsValid()
					|| !Mesh->Info->TryGetStringField(TEXT("format"), Format) || !Format.Equals(TEXT("MRM2"), ESearchCase::CaseSensitive))
				{
					continue; // not a cooked static mesh: its slots are not known
				}
				const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
				const int32 SlotCount = Mesh->Info->TryGetArrayField(TEXT("sections"), Sections) && Sections ? Sections->Num() : 0;
				double Materials = 0.0;
				(*Part)->TryGetNumberField(TEXT("materials"), Materials);
				if (static_cast<int32>(Materials) > SlotCount)
				{
					OutError = FString::Printf(TEXT("Vehicle '%s': parts/%d names %d materials, but mesh %s has %d material slots; name one per slot."),
						*Name, PartIndex, static_cast<int32>(Materials), *MeshId, SlotCount);
					return false;
				}
				const TArray<TSharedPtr<FJsonValue>>* PaintSlots = nullptr;
				if ((*Part)->TryGetArrayField(TEXT("paintSlots"), PaintSlots) && PaintSlots)
				{
					for (const TSharedPtr<FJsonValue>& PaintSlot : *PaintSlots)
					{
						double Slot = 0.0;
						if (PaintSlot.IsValid() && PaintSlot->TryGetNumber(Slot) && static_cast<int32>(Slot) >= SlotCount)
						{
							OutError = FString::Printf(TEXT("Vehicle '%s': parts/%d paints slot %d, but mesh %s has %d material slots (the first is 0)."),
								*Name, PartIndex, static_cast<int32>(Slot), *MeshId, SlotCount);
							return false;
						}
					}
				}
			}
		}
		return true;
	}

	/**
	 * Cooks one file with the local tool and reads back what it wrote, held to the tool's --result file (the only other thing read
	 * back). OutInfo is the result's info object (may be null). Name is the file's name for the creator.
	 */
	bool PreviewRunCook(FExportState& State, const FString& Name, const FMantleAssetUploadOptions& UploadOptions, const TArray<uint8>& Bytes,
		TArray<uint8>& OutCooked, FMantleContentHash& OutHash, TSharedPtr<FJsonObject>& OutInfo, FString& OutError)
	{
		const FString& Kind = UploadOptions.Kind;
		if (Bytes.Num() == 0)
		{
			OutError = FString::Printf(TEXT("'%s' was prepared with no content."), *Name);
			return false;
		}

		const FString Stem = State.WorkDir / FString::Printf(TEXT("cook-%d"), ++State.CookCount);
		const FString InputFile = Stem + TEXT("-in.bin");
		const FString OutputFile = Stem + TEXT("-out.bin");
		const FString ResultFile = Stem + TEXT("-result.json");
		const FString OptionsFile = Stem + TEXT("-options.json");
		FTempFiles Temp;
		Temp.Files = { InputFile, OutputFile, ResultFile, OptionsFile };

		if (!PreviewWriteBytes(InputFile, Bytes))
		{
			OutError = FString::Printf(TEXT("Could not write the working file for '%s' in %s."), *Name, *State.WorkDir);
			return false;
		}
		FString Arguments = FString::Printf(TEXT("--kind=%s --input=%s --output=%s --result=%s"), *Kind,
			*MantlePreviewShared::Quoted(MantlePreviewShared::PlatformPath(InputFile)), *MantlePreviewShared::Quoted(MantlePreviewShared::PlatformPath(OutputFile)), *MantlePreviewShared::Quoted(MantlePreviewShared::PlatformPath(ResultFile)));
		const FString OptionsJson = PreviewCookOptionsJson(UploadOptions);
		if (!OptionsJson.IsEmpty())
		{
			if (!PreviewWriteText(OptionsFile, OptionsJson))
			{
				OutError = FString::Printf(TEXT("Could not write the working file for '%s' in %s."), *Name, *State.WorkDir);
				return false;
			}
			Arguments += FString::Printf(TEXT(" --options=%s"), *MantlePreviewShared::Quoted(MantlePreviewShared::PlatformPath(OptionsFile)));
		}
		// #628: the target client may be newer than the released game, whose gated keys the cook would otherwise refuse.
		if (!State.AllowFeatures.IsEmpty())
		{
			Arguments += FString::Printf(TEXT(" --allow-features=%s"), *State.AllowFeatures);
		}

		uint32 ProcessId = 0;
		FProcHandle Process = FPlatformProcess::CreateProc(*State.CookTool, *Arguments, /*bLaunchDetached*/ false, /*bLaunchHidden*/ true,
			/*bLaunchReallyHidden*/ true, &ProcessId, /*PriorityModifier*/ 0, /*OptionalWorkingDirectory*/ nullptr, /*PipeWriteChild*/ nullptr);
		if (!Process.IsValid())
		{
			OutError = FString::Printf(TEXT("Could not start the cook tool %s for '%s'."), *State.CookTool, *Name);
			return false;
		}
		const double Deadline = FPlatformTime::Seconds() + CookTimeoutSeconds;
		while (FPlatformProcess::IsProcRunning(Process))
		{
			if (FPlatformTime::Seconds() > Deadline)
			{
				FPlatformProcess::TerminateProc(Process, /*KillTree*/ true);
				FPlatformProcess::CloseProc(Process);
				OutError = FString::Printf(TEXT("The cook tool did not finish '%s' within %d minutes, so it was stopped."), *Name, static_cast<int32>(CookTimeoutSeconds / 60.0));
				return false;
			}
			// A long cook must not make Cancel unreachable: this also keeps the dialog drawn.
			if (PreviewCancelled(State))
			{
				FPlatformProcess::TerminateProc(Process, /*KillTree*/ true);
				FPlatformProcess::CloseProc(Process);
				OutError = TEXT("Cancelled.");
				return false;
			}
			FPlatformProcess::Sleep(0.02f);
		}
		int32 ExitCode = -1;
		FPlatformProcess::GetProcReturnCode(Process, &ExitCode);
		FPlatformProcess::CloseProc(Process);

		const TSharedPtr<FJsonObject> Result = PreviewReadJsonFile(ResultFile);
		if (!Result.IsValid())
		{
			OutError = FString::Printf(TEXT("The cook tool wrote no readable result for '%s' (exit code %d)."), *Name, ExitCode);
			return false;
		}
		bool bOk = false;
		Result->TryGetBoolField(TEXT("ok"), bOk);
		if (!bOk || ExitCode != 0)
		{
			FString Reason;
			Result->TryGetStringField(TEXT("error"), Reason);
			if (Reason.IsEmpty())
			{
				Reason = FString::Printf(TEXT("exit code %d"), ExitCode);
			}
			// Exit 1 is the cook refusing the asset; anything else is the tool failing to run.
			OutError = ExitCode == 1 || ExitCode == 0
				? FString::Printf(TEXT("'%s' was refused by the cook: %s"), *Name, *Reason)
				: FString::Printf(TEXT("The cook tool could not process '%s' (exit code %d): %s"), *Name, ExitCode, *Reason);
			return false;
		}

		TArray<uint8>& Cooked = OutCooked;
		Cooked.Reset();
		if (!FFileHelper::LoadFileToArray(Cooked, *OutputFile) || Cooked.Num() == 0)
		{
			OutError = FString::Printf(TEXT("The cook tool said it cooked '%s' but left no output."), *Name);
			return false;
		}
		if (Cooked.Num() > MaxAssetFileBytes)
		{
			OutError = FString::Printf(TEXT("'%s' cooked to %lld bytes; a preview asset may be at most %lld."), *Name, static_cast<int64>(Cooked.Num()), MaxAssetFileBytes);
			return false;
		}
		OutHash = FMantleContentHash::OfBytes(Cooked);
		const FMantleContentHash& Hash = OutHash;
		FString ReportedHash;
		double ReportedBytes = 0.0;
		Result->TryGetStringField(TEXT("cookedHash"), ReportedHash);
		Result->TryGetNumberField(TEXT("cookedBytes"), ReportedBytes);
		if (!ReportedHash.Equals(Hash.ToString(), ESearchCase::CaseSensitive) || static_cast<int64>(ReportedBytes) != Cooked.Num())
		{
			OutError = FString::Printf(TEXT("The cook tool's result for '%s' does not match the file it wrote (%s, %d bytes)."), *Name, *Hash.ToString(), Cooked.Num());
			return false;
		}

		OutInfo.Reset();
		const TSharedPtr<FJsonObject>* Info = nullptr;
		if (Result->TryGetObjectField(TEXT("info"), Info) && Info)
		{
			OutInfo = *Info;
		}
		return true;
	}

	/** Cooks one upload with the local tool (PreviewRunCook) and puts the result in the bundle's Assets/ folder. */
	bool PreviewCookAsset(FExportState& State, const FMantlePlaceUpload& Upload, FBundleAsset& OutAsset, TSharedPtr<FJsonObject>& OutInfo, FString& OutError)
	{
		const FString& Name = Upload.SourceName;
		const FString& Kind = Upload.Options.Kind;
		TArray<uint8> Cooked;
		FMantleContentHash Hash;
		if (!PreviewRunCook(State, Name, Upload.Options, Upload.Bytes, Cooked, Hash, OutInfo, OutError))
		{
			return false;
		}

		OutAsset = FBundleAsset();
		OutAsset.Kind = Kind;
		OutAsset.File = TEXT("Assets/") + Hash.ToHex();
		OutAsset.CookedHash = Hash.ToString();
		OutAsset.CookedBytes = Cooked.Num();
		OutAsset.Role = Upload.Options.Role;

		if (!State.WrittenFiles.Contains(OutAsset.File))
		{
			if (State.TotalAssetBytes + Cooked.Num() > MaxAssetTotalBytes)
			{
				OutError = FString::Printf(TEXT("The preview's assets would pass %lld MiB; '%s' does not fit."), MaxAssetTotalBytes >> 20, *Name);
				return false;
			}
			if (!PreviewWriteBytes(State.BundleDir / OutAsset.File, Cooked))
			{
				OutError = FString::Printf(TEXT("Could not write %s for '%s'."), *OutAsset.File, *Name);
				return false;
			}
			State.WrittenFiles.Add(OutAsset.File);
			State.TotalAssetBytes += Cooked.Num();
		}
		return true;
	}

	/** The most source or compiled JavaScript a script may be (what FMantleQuickJSRuntime::TranspileTypeScript and Publish hold it to). */
	constexpr int64 MaxScriptBytes = 1024 * 1024;

	/** Where PreviewTypeScriptCompilerBundle's shim leaves the source map: after the JavaScript, as a block comment. */
	const TCHAR* const PreviewSourceMapMarker = TEXT("\n/*MANTLE_SOURCE_MAP:");

	/**
	 * The plugin's vendored TypeScript compiler, loaded once, with a shim appended that wraps ts.transpileModule: it
	 * transpiles exactly as the caller asked (so the JavaScript is what Publish uploads), transpiles again with
	 * sourceMap on, and appends that map to the output as a trailing comment (with any "*" "/" in it escaped as the
	 * JSON-valid "*\/"). FMantleQuickJSRuntime::TranspileTypeScript returns only the output text, so this is how the
	 * map comes back without changing it. Empty when the compiler cannot be read.
	 */
	const FString& PreviewTypeScriptCompilerBundle()
	{
		static const FString Bundle = []
		{
			const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MantleCore"));
			if (!Plugin.IsValid())
			{
				return FString();
			}
			const FString Path = Plugin->GetBaseDir() / TEXT("Source/ThirdParty/TypeScriptCompiler/typescript.js");
			FString Source;
			if (!FFileHelper::LoadFileToString(Source, *Path))
			{
				UE_LOG(LogMantle, Error, TEXT("Could not load bundled TypeScript compiler: %s"), *Path);
				return FString();
			}
			// The bundle's last line is a line comment, so the shim starts on a new line.
			Source += TEXT(R"JS(

;(function() {
    var original = ts;
    var originalTranspile = original.transpileModule;
    ts = Object.create(original, {
        transpileModule: { value: function(source, options) {
            var result = originalTranspile(source, options);
            var mappedOptions = Object.assign({}, options);
            mappedOptions.compilerOptions = Object.assign({}, options.compilerOptions, { sourceMap: true });
            var mapped = originalTranspile(source, mappedOptions);
            result.outputText = (result.outputText || "") + "\n/*MANTLE_SOURCE_MAP:"
                + String(mapped.sourceMapText || "").split("*/").join("*\\/") + "*/";
            return result;
        } }
    });
})();
)JS");
			return Source;
		}();
		return Bundle;
	}

	/**
	 * Compiles TypeScript as FMantlePlaceExporter::CompileTypeScript does (same compiler and options, so
	 * OutJavaScript is what Publish uploads as the script's source), and also returns the compiler's v3
	 * source map for that JavaScript.
	 */
	bool PreviewCompileTypeScript(const FString& Source, FString& OutJavaScript, FString& OutSourceMap, FMantleScriptError& OutError)
	{
		OutJavaScript.Reset();
		OutSourceMap.Reset();
		const FTCHARToUTF8 SourceUtf8(*Source);
		if (SourceUtf8.Length() > MaxScriptBytes)
		{
			OutError = FMantleScriptError{ EMantleScriptErrorKind::MemoryBudgetExceeded,
				FString::Printf(TEXT("script source is %d bytes; maximum is %lld"), SourceUtf8.Length(), MaxScriptBytes) };
			return false;
		}
		// The map rides in the output text, so the compile's own size gate gets room for it; the JavaScript is held to the limit below.
		FString Combined;
		if (!FMantleQuickJSRuntime::TranspileTypeScript(PreviewTypeScriptCompilerBundle(), Source, Combined, OutError, static_cast<uint64>(MaxScriptBytes) * 8))
		{
			return false;
		}
		const int32 MarkerAt = Combined.Find(PreviewSourceMapMarker, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (MarkerAt == INDEX_NONE || !Combined.EndsWith(TEXT("*/"), ESearchCase::CaseSensitive))
		{
			OutError = FMantleScriptError{ EMantleScriptErrorKind::Compile, TEXT("the TypeScript compiler returned no source map") };
			return false;
		}
		const int32 MapStart = MarkerAt + FCString::Strlen(PreviewSourceMapMarker);
		OutSourceMap = Combined.Mid(MapStart, Combined.Len() - 2 - MapStart);
		OutSourceMap.ReplaceInline(TEXT("*\\/"), TEXT("*/"), ESearchCase::CaseSensitive);
		OutJavaScript = Combined.Left(MarkerAt);

		const FTCHARToUTF8 JavaScriptUtf8(*OutJavaScript);
		if (JavaScriptUtf8.Length() > MaxScriptBytes)
		{
			OutError = FMantleScriptError{ EMantleScriptErrorKind::MemoryBudgetExceeded,
				FString::Printf(TEXT("script source is %d bytes; maximum is %lld"), JavaScriptUtf8.Length(), MaxScriptBytes) };
			OutJavaScript.Reset();
			OutSourceMap.Reset();
			return false;
		}
		TSharedPtr<FJsonObject> Map;
		FString Mappings;
		double Version = 0.0;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(OutSourceMap), Map) || !Map.IsValid()
			|| !Map->TryGetNumberField(TEXT("version"), Version) || static_cast<int32>(Version) != 3 || !Map->TryGetStringField(TEXT("mappings"), Mappings))
		{
			OutError = FMantleScriptError{ EMantleScriptErrorKind::Compile, TEXT("the TypeScript compiler returned a source map that is not a version 3 map") };
			OutJavaScript.Reset();
			OutSourceMap.Reset();
			return false;
		}
		return true;
	}

	/** The next generation number in a session's folder: one more than the highest numbered folder there. */
	int32 PreviewNextGeneration(const FString& SessionDir)
	{
		TArray<FString> Existing;
		IFileManager::Get().FindFiles(Existing, *(SessionDir / TEXT("*")), /*Files*/ false, /*Directories*/ true);
		int32 Highest = 0;
		for (const FString& Name : Existing)
		{
			if (Name.IsNumeric())
			{
				Highest = FMath::Max(Highest, FCString::Atoi(*Name));
			}
		}
		return Highest + 1;
	}

	/** The side name of a script path's top folder, or empty when it is not Server, Client or Shared. */
	FString PreviewSideOf(const FString& Path)
	{
		if (Path.StartsWith(TEXT("Server/"), ESearchCase::CaseSensitive)) return TEXT("server");
		if (Path.StartsWith(TEXT("Client/"), ESearchCase::CaseSensitive)) return TEXT("client");
		if (Path.StartsWith(TEXT("Shared/"), ESearchCase::CaseSensitive)) return TEXT("shared");
		return FString();
	}

	void PreviewAddIssueLines(const TArray<FMantleValidationIssue>& Issues, TArray<FString>& OutErrors, TArray<FString>& OutWarnings)
	{
		for (const FMantleValidationIssue& Issue : Issues)
		{
			(Issue.Severity == EMantleIssueSeverity::Error ? OutErrors : OutWarnings).Add(Issue.ToString());
		}
	}

	/** The export proper. Returns false with Result.Errors set; Run deletes the generation folder and the working folder. */
	bool PreviewExportBundle(UWorld* World, const FMantlePreviewExportOptions& Options, FExportState& State, FMantlePreviewExportResult& Result)
	{
		// --- A saved level in the editor ----------------------------------------------------------
		if (!World)
		{
			Result.Errors.Add(TEXT("There is no level open to export."));
			return false;
		}
		if (World->WorldType != EWorldType::Editor)
		{
			Result.Errors.Add(TEXT("The preview export reads the level open in the editor, not a game or preview world."));
			return false;
		}
		const FString PackageName = World->GetOutermost()->GetName();
		if (!PackageName.StartsWith(TEXT("/Game/")) || !FPackageName::DoesPackageExist(PackageName))
		{
			Result.Errors.Add(FString::Printf(TEXT("Save the level under /Game first ('%s' is not a saved level); the preview reads its scripts and UI files from beside the saved level."), *PackageName));
			return false;
		}
		// #628: everything below converts, cooks and exports for this client's features (the conversion's foliage and grass hooks read it).
		const FMantleTargetFeaturesScope TargetScope(Options.Features);
		State.AllowFeatures = Options.Features.ToArgument();
		FString CookToolError;
		if (!FMantlePreviewExport::FindCookTool(Options.CookTool, State.CookTool, CookToolError))
		{
			Result.Errors.Add(CookToolError);
			return false;
		}

		// --- #729: weapons from the weapon builder, read before anything is cooked ------------------------
		struct FLoadedWeapon
		{
			const FMantleWeaponToTry* Weapon = nullptr;
			USkeletalMesh* Mesh = nullptr;
			FString DocumentText;
			FString MeshName;
		};
		TArray<FLoadedWeapon> LoadedWeapons;
		if (Options.Weapons.Num() > 0)
		{
			if (!FMantlePreviewExport::ClientReadsWeapons(Options.Features))
			{
				Result.Errors.Add(FMantlePreviewExport::WeaponsNeedNewerClientMessage());
				return false;
			}
			for (const FMantleWeaponToTry& Weapon : Options.Weapons)
			{
				FLoadedWeapon& Loaded = LoadedWeapons.AddDefaulted_GetRef();
				Loaded.Weapon = &Weapon;
				Loaded.Mesh = LoadObject<USkeletalMesh>(nullptr, *Weapon.MeshObjectPath);
				if (!Loaded.Mesh)
				{
					Result.Errors.Add(FString::Printf(TEXT("The 3D model of your weapon %s was not found. Press Create weapon again, then try Test locally."), *Weapon.Name));
					return false;
				}
				if (!FFileHelper::LoadFileToString(Loaded.DocumentText, *Weapon.DocumentFile))
				{
					Result.Errors.Add(FString::Printf(TEXT("The settings file of your weapon %s was not found. Press Create weapon again, then try Test locally."), *Weapon.Name));
					return false;
				}
				Loaded.MeshName = FMantlePreviewExport::ReadWeaponMeshName(Loaded.DocumentText);
				if (Loaded.MeshName.IsEmpty() || !MantlePreviewLaunch::IsBundleRelativePath(Weapon.BundleFile()))
				{
					Result.Errors.Add(FString::Printf(TEXT("The settings file of your weapon %s does not name its 3D model. Press Create weapon again, then try Test locally."), *Weapon.Name));
					return false;
				}
			}
		}

		// --- Validate exactly as Publish does -------------------------------------------------------
		if (PreviewStageCancelled(State, ProgressValidate, TEXT("Checking the level")))
		{
			return false;
		}
		const FMantleValidationResult Validation = FMantlePlaceValidator::Validate(World);
		PreviewAddIssueLines(Validation.Issues, Result.Errors, Result.Warnings);
		if (Validation.HasErrors())
		{
			// #628: a level that fails Check still says what an export would keep, approximate and drop.
			FMantleExportOptions RefusedOptions;
			RefusedOptions.PlaceId = TEXT("plc_preview01");
			RefusedOptions.Name = World->GetName();
			RefusedOptions.AuthorId = TEXT("usr_preview");
			Result.Coverage = FMantlePlaceExporter::Export(World, RefusedOptions).Coverage;
			Result.Coverage.Log();
			return false;
		}

		// --- Gather and convert every dependency (what Publish converts, with nothing sent) ---------
		if (PreviewStageCancelled(State, ProgressConvert, TEXT("Converting the level's assets")))
		{
			return false;
		}
		FMantlePlaceDependencies Dependencies = FMantlePlaceDependencies::Gather(World);
		// #729: a weapon's model is a skeletal mesh no actor in the level shows; it goes through the same gather, convert and cook.
		for (const FLoadedWeapon& Loaded : LoadedWeapons)
		{
			Dependencies.AddSkeletalMesh(Loaded.Mesh, FString::Printf(TEXT("weapon %s"), *Loaded.Weapon->Name));
		}
		FMantlePlaceAssetConverter Converter;
		FMantlePlaceAssetSet Set;
		Converter.Convert(Dependencies, Set);

		TArray<FMantleUIDocumentFile> UIFiles;
		TArray<FMantleUIImageFile> UIImageFiles;
		FMantlePlaceExporter::CollectUIDocumentFiles(FMantlePlaceExporter::GetUIFolder(World), UIFiles, UIImageFiles);
		const TArray<FMantleUIDocumentProblem> UIProblems = FMantlePlaceExporter::CheckUIDocumentFiles(UIFiles);
		if (UIProblems.Num() > 0)
		{
			for (const FMantleUIDocumentProblem& Problem : UIProblems)
			{
				Result.Errors.Add(FString::Printf(TEXT("%s%s %s"), Problem.Path.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("'%s' "), *Problem.Path), *Problem.Problem, *Problem.Remedy));
			}
			return false;
		}
		// The UI/ folder's images take the texture upload a decal's texture takes.
		for (const FMantleUIImageFile& Image : UIImageFiles)
		{
			Converter.ConvertUIImage(Image.Path, Image.DiskPath, Set);
		}
		PreviewAddIssueLines(Set.Issues, Result.Errors, Result.Warnings);
		if (Set.HasErrors())
		{
			return false;
		}

		// --- The generation folder -------------------------------------------------------------------
		const FString Root = Options.OutputRoot.IsEmpty() ? FMantlePreviewExport::DefaultOutputRoot() : FPaths::ConvertRelativePathToFull(Options.OutputRoot);
		const FString SessionDir = Root / PreviewProcessSession();
		const int32 Generation = PreviewNextGeneration(SessionDir);
		const FString BundleDir = SessionDir / FString::FromInt(Generation);
		if (IFileManager::Get().DirectoryExists(*BundleDir))
		{
			Result.Errors.Add(FString::Printf(TEXT("The preview folder %s already exists; an export never writes into an existing generation."), *BundleDir));
			return false;
		}
		if (!IFileManager::Get().MakeDirectory(*BundleDir, true))
		{
			Result.Errors.Add(FString::Printf(TEXT("Could not create the preview folder %s."), *BundleDir));
			return false;
		}
		State.BundleDir = BundleDir;
		State.WorkDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Mantle") / TEXT("PreviewCook") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
		if (!IFileManager::Get().MakeDirectory(*State.WorkDir, true))
		{
			Result.Errors.Add(FString::Printf(TEXT("Could not create the working folder %s."), *State.WorkDir));
			return false;
		}

		// --- Cook every upload, in Publish's order, recording each provisional id ----------------------
		// Textures, then materials (their documents name the texture ids), meshes, skeletal meshes, clips, sounds,
		// heightfields, then vehicles (their documents name the materials, meshes, liveries and horn sound), then weapons (#789: their
		// documents name the skeletal mesh and materials).
		const EMantlePlaceUploadStage Stages[] = {
			EMantlePlaceUploadStage::Texture, EMantlePlaceUploadStage::Material, EMantlePlaceUploadStage::Mesh,
			EMantlePlaceUploadStage::SkeletalMesh, EMantlePlaceUploadStage::AnimClip, EMantlePlaceUploadStage::Sound,
			EMantlePlaceUploadStage::Heightfield, EMantlePlaceUploadStage::Vehicle, EMantlePlaceUploadStage::Weapon };
		const TCHAR* const StageNames[] = { TEXT("texture"), TEXT("material"), TEXT("mesh"), TEXT("skeletal mesh"), TEXT("animation"), TEXT("sound"), TEXT("heightfield"), TEXT("vehicle"), TEXT("weapon") };
		static_assert(UE_ARRAY_COUNT(Stages) == UE_ARRAY_COUNT(StageNames), "one name per cook stage");
		// A level with Weapon Spawns (#789) is tried on an avatar body, as a single weapon is (below).
		bool bLevelHasWeapons = false;
		for (int32 StageIndex = 0; StageIndex < UE_ARRAY_COUNT(Stages); ++StageIndex)
		{
			const EMantlePlaceUploadStage Stage = Stages[StageIndex];
			if (PreviewStageCancelled(State, ProgressCookStages / static_cast<float>(UE_ARRAY_COUNT(Stages)), FString::Printf(TEXT("Cooking %s assets"), StageNames[StageIndex])))
			{
				return false;
			}
			// Built per stage: a material's and a vehicle's bytes name ids the earlier stages have just recorded.
			TArray<FMantlePlaceUpload> Uploads;
			FString Error;
			if (!Set.TakeUploads(Stage, Uploads, Error))
			{
				Result.Errors.Add(Error);
				return false;
			}
			bLevelHasWeapons |= Stage == EMantlePlaceUploadStage::Weapon && Uploads.Num() > 0;
			// This stage's share of the bar, one step per cook ("Cooking mesh 3 of 12").
			FScopedSlowTask CookTask(static_cast<float>(Uploads.Num()));
			int32 CookIndex = 0;
			for (const FMantlePlaceUpload& Upload : Uploads)
			{
				CookTask.EnterProgressFrame(1.f, FText::FromString(FString::Printf(TEXT("Cooking %s %d of %d: %s"), StageNames[StageIndex], ++CookIndex, Uploads.Num(), *Upload.SourceName)));
				if (PreviewCancelled(State))
				{
					return false;
				}
				TMap<FString, FString> MaterialTextures;
				if (Stage == EMantlePlaceUploadStage::Material && !PreviewCheckMaterialTextures(State, Upload.SourceName, Upload.Bytes, MaterialTextures, Error))
				{
					Result.Errors.Add(Error);
					return false;
				}
				if (State.Assets.Num() >= MaxAssets)
				{
					Result.Errors.Add(FString::Printf(TEXT("This place needs more than %d assets; a preview holds at most that many."), MaxAssets));
					return false;
				}

				FBundleAsset Asset;
				TSharedPtr<FJsonObject> Info;
				if (!PreviewCookAsset(State, Upload, Asset, Info, Error))
				{
					Result.Errors.Add(Error);
					return false;
				}
				if (Stage == EMantlePlaceUploadStage::Vehicle && !PreviewCheckVehicleNames(State, Upload.SourceName, Info, Error))
				{
					Result.Errors.Add(Error);
					return false;
				}
				Asset.Id = FMantlePlaceAssets::ProvisionalId(Upload.Key);
				Asset.Textures = MoveTemp(MaterialTextures);
				Asset.Info = Info;
				UE_LOG(LogMantle, Display, TEXT("MANTLE_PREVIEW_EXPORT cooked %s '%s' -> %s"), *Asset.Kind, *Upload.SourceName, *Asset.Id);

				Set.RecordUpload(Upload.Key, Asset.Id, Upload.Hash, Upload.Bytes.Num());
				State.AssetIndexById.Add(Asset.Id, State.Assets.Num());
				State.Assets.Add(MoveTemp(Asset));
			}
		}

		// --- The place, with the provisional ids and src_ ids the exporter writes offline --------------
		if (PreviewStageCancelled(State, ProgressScripts, TEXT("Writing the place, scripts and UI")))
		{
			return false;
		}
		FMantleExportOptions ExportOptions;
		ExportOptions.PlaceId = TEXT("plc_preview01");
		ExportOptions.Name = World->GetName();
		ExportOptions.AuthorId = TEXT("usr_preview");
		ExportOptions.Assets = &Set;
		const FMantleExportResult Exported = FMantlePlaceExporter::Export(World, ExportOptions);
		// The materials it gathered for the cook, recognised, baked or lost, join the report (Export leaves them to whoever gathered).
		Result.Coverage = Exported.Coverage;
		FMantleCoverageSurvey::AddMaterials(Dependencies, Result.Coverage);
		Options.Features.ReportMaterialDowngrades(Set, Result.Coverage);
		Result.Coverage.Log();
		if (!Exported.bSuccess)
		{
			for (const FMantleParseError& Error : Exported.Errors)
			{
				Result.Errors.Add(Error.ToString());
			}
			if (Exported.Errors.Num() == 0)
			{
				Result.Errors.Add(TEXT("The level could not be exported."));
			}
			return false;
		}
		// Every uploaded id the place names has to be a file in the bundle, or the client could not build it.
		for (const FMantleAssetRef& Ref : Exported.Place.Assets)
		{
			if (FMantlePreparedPlaceAssets::IsUploadedId(Ref.Id) && !State.AssetIndexById.Contains(Ref.Id))
			{
				Result.Errors.Add(FString::Printf(TEXT("The place uses the uploaded asset %s, which this level did not convert, so a preview cannot supply it."), *Ref.Id));
				return false;
			}
		}
		if (Exported.Place.Storage.Num() > 0)
		{
			Result.Warnings.Add(TEXT("This place stores library items (Place Settings > Storage); a preview cannot fetch them, so it runs without."));
		}
		// #838: the level's script packages run from the bundle as stored ones do (mounted at Packages/<Name>/). One no storage entry
		// names yet gets a placeholder entry, so its Script Points parse; publishing stores the real item.
		TArray<FMantleLocalScriptPackage> LocalPackages;
		FMantlePlace PreviewPlace = Exported.Place;
		{
			TArray<FMantleScriptPackageProblem> PackageProblems;
			FMantleScriptPackages::Collect(FMantleScriptPackages::GetPackagesFolder(World), LocalPackages, PackageProblems);
			for (const FMantleScriptPackageProblem& Problem : PackageProblems)
			{
				if (!Problem.bError)
				{
					Result.Warnings.Add(Problem.Problem + TEXT(" ") + Problem.Remedy);
				}
			}
			FMantleScriptPackages::AddPreviewStorage(PreviewPlace.Storage, LocalPackages);
			if (PreviewPlace.Storage.Num() > FMantlePlaceLimits::MaxStorageEntries)
			{
				Result.Errors.Add(FString::Printf(TEXT("This place would store %d library items with its script packages; a place stores at most %d. Remove some from Place Settings > Storage."),
					PreviewPlace.Storage.Num(), FMantlePlaceLimits::MaxStorageEntries));
				return false;
			}
		}
		// The ids scripts use for the Script Assets in this preview. Provisional: Publish assigns others, and
		// the settings actor's Script Asset Ids list stays for those (it says "from the last publish").
		for (const FMantleScriptAssetId& ScriptAsset : Exported.ScriptAssets)
		{
			UE_LOG(LogMantle, Display, TEXT("MANTLE_PREVIEW_EXPORT script asset %s = %s"), *ScriptAsset.Path, *ScriptAsset.Id);
			Result.Warnings.Add(FString::Printf(TEXT("Script asset %s = %s (preview id)"), *ScriptAsset.Path, *ScriptAsset.Id));
		}
		if (Exported.ScriptAssets.Num() > 0)
		{
			Result.Warnings.Add(TEXT("The ids above are for this preview only: Publish gives each Script Asset a different id (logged as \"Script asset <path> = <id>\"), so a script that spells one out needs the published id after publishing."));
		}
		if (!PreviewWriteText(State.BundleDir / TEXT("place.json"), FMantlePlaceExporter::ToJson(PreviewPlace)))
		{
			Result.Errors.Add(TEXT("Could not write place.json."));
			return false;
		}
		// #1011: the creator's local test secrets go next to place.json, where the preview reads them. TestLocally deletes the copy
		// when the preview ends; it is never part of anything uploaded.
		{
			FString SecretsError;
			if (!MantlePlaceSecrets::CopyToBundle(MantlePlaceSecrets::LocalFilePathFor(World->GetName()), State.BundleDir, SecretsError))
			{
				Result.Errors.Add(SecretsError);
				return false;
			}
		}
		// #628: the coverage report beside the manifest. Not part of the bundle's contract, so a failure to write it is a warning.
		if (PreviewWriteText(State.BundleDir / TEXT("coverage.json"), Result.Coverage.ToJson()))
		{
			Result.CoverageFile = State.BundleDir / TEXT("coverage.json");
		}
		else
		{
			Result.Warnings.Add(TEXT("Could not write coverage.json, the export's coverage report."));
		}

		// --- Scripts: the sources Publish uploads, TypeScript compiled, graphs as their Luau ------------
		TArray<FBundleScript> Scripts;
		{
			const FString ScriptsFolder = FMantlePlaceExporter::GetScriptsFolder(World);
			TArray<FMantleScriptCatalogEntry> Entries;
			TArray<FMantleScriptCollectionIssue> Issues;
			FMantlePlaceExporter::CollectScriptFiles(ScriptsFolder, Entries, Issues);
			TArray<FMantleGraphScriptEntry> GraphEntries;
			TArray<FMantleParseError> GraphErrors;
			TArray<FString> GraphPaths;
			FMantlePlaceExporter::CollectGraphScripts(ScriptsFolder, GraphEntries, GraphErrors, GraphPaths);
			FMantlePlaceExporter::FilterGraphGeneratedFiles(Entries, Issues, GraphPaths);
			if (GraphErrors.Num() > 0)
			{
				for (const FMantleParseError& Error : GraphErrors)
				{
					Result.Errors.Add(FString::Printf(TEXT("Could not compile graph '%s': %s"), *Error.Path.Mid(FString(TEXT("/scripts/")).Len()), *Error.Message));
				}
				return false;
			}
			for (const FMantleScriptCollectionIssue& Issue : Issues)
			{
				Result.Warnings.Add(FString::Printf(TEXT("Scripts/%s is not a script Mantle runs, so the preview leaves it out."), *Issue.RelativePath));
			}

			auto AddScript = [&Scripts, &Result](const FString& Path, const FString& Text, const FString& SourceMap)
			{
				FBundleScript& Script = Scripts.AddDefaulted_GetRef();
				Script.Path = Path;
				Script.Side = PreviewSideOf(Path);
				Script.File = TEXT("Scripts/") + Path;
				Script.Text = Text;
				if (!SourceMap.IsEmpty())
				{
					Script.MapFile = TEXT("Maps/") + Path + TEXT(".map");
					Script.MapText = SourceMap;
				}
				if (Script.Side.IsEmpty())
				{
					Result.Errors.Add(FString::Printf(TEXT("Script '%s' is not under Server/, Client/ or Shared/."), *Path));
					return false;
				}
				return true;
			};
			for (const FMantleScriptCatalogEntry& Entry : Entries)
			{
				if (Entry.Language != EMantleScriptCatalogLanguage::Luau && Entry.Language != EMantleScriptCatalogLanguage::JavaScript
					&& Entry.Language != EMantleScriptCatalogLanguage::TypeScript && Entry.Language != EMantleScriptCatalogLanguage::Python)
				{
					Result.Errors.Add(FString::Printf(TEXT("'%s' has a language Mantle cannot publish yet."), *Entry.Path));
					return false;
				}
				FString Text = Entry.Source;
				FString SourceMap;
				if (Entry.Language == EMantleScriptCatalogLanguage::TypeScript)
				{
					// What Publish uploads as the script's source (the client runs it as it runs a published one),
					// and the compiler's source map for it, which the client reads to report a thrown error on the .ts line.
					FMantleScriptError CompileError;
					if (!PreviewCompileTypeScript(Entry.Source, Text, SourceMap, CompileError))
					{
						Result.Errors.Add(FString::Printf(TEXT("Could not compile TypeScript '%s': %s"), *Entry.Path, *CompileError.Message));
						return false;
					}
					// The client refuses a map over the script cap, so a big one is left out rather than failing the preview.
					if (FTCHARToUTF8(*SourceMap).Length() > MaxScriptBytes)
					{
						Result.Warnings.Add(FString::Printf(TEXT("'%s': its source map is over %lld bytes, so this preview reports its errors on JavaScript lines."),
							*Entry.Path, MaxScriptBytes));
						SourceMap.Reset();
					}
				}
				if (!AddScript(Entry.Path, Text, SourceMap))
				{
					return false;
				}
			}
			for (const FMantleGraphScriptEntry& Entry : GraphEntries)
			{
				// The graph's generated Luau, as it sits beside the .graph in a disk catalogue.
				if (!AddScript(FPaths::ChangeExtension(Entry.Path, TEXT("luau")), Entry.CompiledLuau, FString()))
				{
					return false;
				}
			}
			// #838: each package's files at Packages/<Name>/<path>, its side from the folder inside the package, TypeScript compiled
			// with its source map as a place script's is.
			for (const FMantleLocalScriptPackage& Package : LocalPackages)
			{
				for (const FMantleScriptCatalogEntry& Entry : Package.Entries)
				{
					FString Text = Entry.Source;
					FString SourceMap;
					if (Entry.Language == EMantleScriptCatalogLanguage::TypeScript)
					{
						FMantleScriptError CompileError;
						if (!PreviewCompileTypeScript(Entry.Source, Text, SourceMap, CompileError))
						{
							Result.Errors.Add(FString::Printf(TEXT("Could not compile TypeScript '%s': %s"), *Entry.Path, *CompileError.Message));
							return false;
						}
						if (FTCHARToUTF8(*SourceMap).Length() > MaxScriptBytes)
						{
							SourceMap.Reset();
						}
					}
					FBundleScript& Script = Scripts.AddDefaulted_GetRef();
					Script.Path = Entry.Path;
					Script.Side = PreviewSideOf(FMantleScriptPackages::PathInPackage(Entry.Path));
					Script.File = TEXT("Scripts/") + Entry.Path;
					Script.Text = Text;
					if (!SourceMap.IsEmpty())
					{
						Script.MapFile = TEXT("Maps/") + Entry.Path + TEXT(".map");
						Script.MapText = SourceMap;
					}
					if (Script.Side.IsEmpty())
					{
						Result.Errors.Add(FString::Printf(TEXT("Script '%s' is not under its package's Server/, Client/ or Shared/."), *Entry.Path));
						return false;
					}
				}
			}
			for (const FBundleScript& Script : Scripts)
			{
				if (!PreviewWriteText(State.BundleDir / Script.File, Script.Text))
				{
					Result.Errors.Add(FString::Printf(TEXT("Could not write %s."), *Script.File));
					return false;
				}
				if (!Script.MapFile.IsEmpty()
					&& (!MantlePreviewLaunch::IsBundleRelativePath(Script.MapFile) || !PreviewWriteText(State.BundleDir / Script.MapFile, Script.MapText)))
				{
					Result.Errors.Add(FString::Printf(TEXT("Could not write %s."), *Script.MapFile));
					return false;
				}
			}
			// Every script the place lists has to run in the preview.
			for (const FMantleScript& PlaceScript : Exported.Place.Scripts)
			{
				const FString Wanted = PlaceScript.Path.EndsWith(TEXT(".graph")) ? FPaths::ChangeExtension(PlaceScript.Path, TEXT("luau")) : PlaceScript.Path;
				if (!Scripts.ContainsByPredicate([&Wanted](const FBundleScript& Script) { return Script.Path.Equals(Wanted, ESearchCase::IgnoreCase); }))
				{
					Result.Errors.Add(FString::Printf(TEXT("The place lists script '%s', which the export did not find to bundle."), *PlaceScript.Path));
					return false;
				}
			}
		}

		// --- UI documents: .rml and .rcss text (their images are assets above) -----------------------------
		TArray<FBundleUIDocument> UIDocuments;
		for (const FMantleUIDocumentFile& File : UIFiles)
		{
			FBundleUIDocument& Document = UIDocuments.AddDefaulted_GetRef();
			Document.Path = File.Path;
			Document.File = File.Path;
			if (!MantlePreviewLaunch::IsBundleRelativePath(Document.File) || !PreviewWriteText(State.BundleDir / Document.File, File.Text))
			{
				Result.Errors.Add(FString::Printf(TEXT("Could not write the UI file '%s' into the bundle."), *File.Path));
				return false;
			}
		}
		for (const FMantleUIDocument& PlaceDocument : Exported.Place.UIDocuments)
		{
			if (!PlaceDocument.IsImage()
				&& !UIDocuments.ContainsByPredicate([&PlaceDocument](const FBundleUIDocument& Document) { return Document.Path.Equals(PlaceDocument.Path, ESearchCase::IgnoreCase); }))
			{
				Result.Errors.Add(FString::Printf(TEXT("The place lists the UI file '%s', which the export did not find to bundle."), *PlaceDocument.Path));
				return false;
			}
		}

		// --- #729: weapons: the settings file as text, and the model's cooked asset id ----------------------
		TArray<FMantlePreviewBundleWeapon> BundleWeapons;
		for (const FLoadedWeapon& Loaded : LoadedWeapons)
		{
			// The cook recorded the model under its provisional id, which the key alone gives.
			const FString MeshId = FMantlePlaceAssets::ProvisionalId(FMantlePlaceAssets::SkeletalMeshKey(Loaded.Mesh));
			if (!State.AssetIndexById.Contains(MeshId))
			{
				Result.Errors.Add(FString::Printf(TEXT("The 3D model of your weapon %s could not be prepared for the test."), *Loaded.Weapon->Name));
				return false;
			}
			if (!PreviewWriteText(State.BundleDir / Loaded.Weapon->BundleFile(), Loaded.DocumentText))
			{
				Result.Errors.Add(FString::Printf(TEXT("Could not write the settings file of your weapon %s into the test."), *Loaded.Weapon->Name));
				return false;
			}
			FMantlePreviewBundleWeapon& Entry = BundleWeapons.AddDefaulted_GetRef();
			Entry.File = Loaded.Weapon->BundleFile();
			Entry.Assets.Add(Loaded.MeshName, MeshId);
			// The cooked mesh carries no material ids: the slot's own material (else the default surface), as the exporter fills a
			// SkeletalMesh entity's materials. A library material is not an uploaded asset, so its slot is left alone.
			const TArray<FSkeletalMaterial>& MeshMaterials = Loaded.Mesh->GetMaterials();
			for (const int32 Slot : FMantlePlaceAssets::SkeletalUploadSlots(Loaded.Mesh))
			{
				UMaterialInterface* Material = MeshMaterials.IsValidIndex(Slot) ? MeshMaterials[Slot].MaterialInterface.Get() : nullptr;
				if (!Material)
				{
					Material = UMaterial::GetDefaultMaterial(MD_Surface);
				}
				FString MaterialId;
				if (!FMantlePlaceAssets::IsLibraryAsset(Material))
				{
					MaterialId = FMantlePlaceAssets::ProvisionalId(FMantlePlaceAssets::MaterialKey(Material));
					if (!State.AssetIndexById.Contains(MaterialId))
					{
						Result.Errors.Add(FString::Printf(TEXT("A material of your weapon %s could not be prepared for the test."), *Loaded.Weapon->Name));
						return false;
					}
				}
				Entry.Materials.Add(MaterialId);
			}
			if (Entry.Materials.Num() > 64)
			{
				Result.Errors.Add(FString::Printf(TEXT("Your weapon %s has more than 64 materials, which is more than a test can hold."), *Loaded.Weapon->Name));
				return false;
			}
			Entry.bEquip = Loaded.Weapon->bEquip;
		}

		// --- The creator's chosen avatar -------------------------------------------------------------------
		FString AvatarDescriptor;
		FString AvatarBuiltin;
		FString AvatarError;
		if (Options.DraftAvatar.IsValid())
		{
			// Chunk 9: the avatar the creator just checked, cooked here like the upload worker would; no catalog call.
			const FMantleCosmeticPackage& Draft = *Options.DraftAvatar;
			TArray<MantlePreviewAvatarExport::FCookedDraftAsset> Cooked;
			for (const FMantleCosmeticUploadAsset& Asset : Draft.Assets)
			{
				if (Asset.Key == Draft.ThumbnailKey)
				{
					continue;
				}
				MantlePreviewAvatarExport::FCookedDraftAsset& Out = Cooked.AddDefaulted_GetRef();
				Out.Key = Asset.Key;
				FMantleContentHash Hash;
				FString CookError;
				if (!PreviewRunCook(State, Asset.Label.IsEmpty() ? Asset.Key : Asset.Label, Asset.Options, Asset.Bytes, Out.Bytes, Hash, Out.Info, CookError))
				{
					Result.Errors.Add(CookError);
					return false;
				}
			}
			if (!MantlePreviewAvatarExport::WriteLocalDraft(State.BundleDir, Draft, Cooked, AvatarDescriptor, AvatarBuiltin, AvatarError))
			{
				Result.Errors.Add(AvatarError);
				return false;
			}
			Result.bHasAvatar = true;
		}
		else if (MantlePreviewAvatarExport::Write(State.BundleDir, AvatarDescriptor, AvatarBuiltin, AvatarError, /*bUseOwnAvatarWhenUnset*/ BundleWeapons.Num() > 0 || bLevelHasWeapons))
		{
			Result.bHasAvatar = true;
		}
		else if (!AvatarError.IsEmpty())
		{
			Result.Errors.Add(AvatarError);
			return false;
		}

		// --- The manifest, last: pending, validated, then renamed ---------------------------------------------
		if (PreviewStageCancelled(State, ProgressManifest, TEXT("Writing the manifest")))
		{
			return false;
		}
		FString ManifestJson;
		{
			const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ManifestJson);
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("schemaVersion"), 2);
			Writer->WriteValue(TEXT("placeFile"), FString(TEXT("place.json")));
			Writer->WriteArrayStart(TEXT("scripts"));
			for (const FBundleScript& Script : Scripts)
			{
				Writer->WriteObjectStart();
				Writer->WriteValue(TEXT("path"), Script.Path);
				Writer->WriteValue(TEXT("side"), Script.Side);
				Writer->WriteValue(TEXT("file"), Script.File);
				if (!Script.MapFile.IsEmpty())
				{
					Writer->WriteValue(TEXT("sourceMap"), Script.MapFile);
				}
				Writer->WriteObjectEnd();
			}
			Writer->WriteArrayEnd();
			if (UIDocuments.Num() > 0)
			{
				Writer->WriteArrayStart(TEXT("uiDocuments"));
				for (const FBundleUIDocument& Document : UIDocuments)
				{
					Writer->WriteObjectStart();
					Writer->WriteValue(TEXT("path"), Document.Path);
					Writer->WriteValue(TEXT("file"), Document.File);
					Writer->WriteObjectEnd();
				}
				Writer->WriteArrayEnd();
			}
			if (State.Assets.Num() > 0)
			{
				Writer->WriteArrayStart(TEXT("assets"));
				for (const FBundleAsset& Asset : State.Assets)
				{
					Writer->WriteObjectStart();
					Writer->WriteValue(TEXT("id"), Asset.Id);
					Writer->WriteValue(TEXT("kind"), Asset.Kind);
					Writer->WriteValue(TEXT("file"), Asset.File);
					Writer->WriteValue(TEXT("cookedHash"), Asset.CookedHash);
					Writer->WriteValue(TEXT("cookedBytes"), Asset.CookedBytes);
					if (!Asset.Role.IsEmpty())
					{
						Writer->WriteValue(TEXT("role"), Asset.Role);
					}
					if (Asset.Textures.Num() > 0)
					{
						TArray<FString> Slots;
						Asset.Textures.GetKeys(Slots);
						Slots.Sort();
						Writer->WriteObjectStart(TEXT("textures"));
						for (const FString& Slot : Slots)
						{
							Writer->WriteValue(Slot, Asset.Textures.FindChecked(Slot));
						}
						Writer->WriteObjectEnd();
					}
					Writer->WriteObjectEnd();
				}
				Writer->WriteArrayEnd();
			}
			if (BundleWeapons.Num() > 0)
			{
				Writer->WriteRawJSONValue(TEXT("weapons"), FMantlePreviewExport::WeaponsToJson(BundleWeapons));
			}
			// #838: the packages' manifests, so the client types and defaults Script Point parameters as a published place does.
			const FString PackagesJson = FMantlePreviewExport::PackagesToJson(LocalPackages);
			if (!PackagesJson.IsEmpty())
			{
				Writer->WriteRawJSONValue(TEXT("packages"), PackagesJson);
			}
			if (Result.bHasAvatar)
			{
				Writer->WriteObjectStart(TEXT("avatar"));
				Writer->WriteValue(TEXT("descriptor"), AvatarDescriptor);
				Writer->WriteValue(TEXT("builtin"), AvatarBuiltin);
				Writer->WriteObjectEnd();
			}
			Writer->WriteObjectEnd();
			Writer->Close();
		}
		const FString PendingFile = State.BundleDir / TEXT("manifest.pending.json");
		const FString ManifestFile = State.BundleDir / TEXT("manifest.json");
		if (!PreviewWriteText(PendingFile, ManifestJson))
		{
			Result.Errors.Add(TEXT("Could not write the preview manifest."));
			return false;
		}
		const FMantlePreviewManifest Loaded = MantlePreviewLaunch::LoadManifest(FPaths::ConvertRelativePathToFull(PendingFile));
		if (!Loaded.bSuccess)
		{
			Result.Errors.Add(FString::Printf(TEXT("The preview bundle did not validate: %s"), *Loaded.Error));
			return false;
		}
		if (!IFileManager::Get().Move(*ManifestFile, *PendingFile))
		{
			Result.Errors.Add(TEXT("Could not move manifest.pending.json to manifest.json."));
			return false;
		}

		Result.BundleDirectory = State.BundleDir;
		Result.ManifestFile = ManifestFile;
		Result.NumAssets = State.Assets.Num();
		Result.NumScripts = Scripts.Num();
		Result.NumUIDocuments = UIDocuments.Num();
		Result.NumWeapons = BundleWeapons.Num();
		return true;
	}
}

FString FMantlePreviewExport::DefaultOutputRoot()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Mantle") / TEXT("Preview"));
}

FMantleWeaponToTry FMantleWeaponToTry::FromBuilderName(const FString& SafeName)
{
	FMantleWeaponToTry Weapon;
	Weapon.Name = SafeName;
	Weapon.MeshObjectPath = FString::Printf(TEXT("/Game/MantleWeapons/%s/SKM_%s.SKM_%s"), *SafeName, *SafeName, *SafeName);
	Weapon.DocumentFile = FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / TEXT("MantleWeapons") / SafeName / (SafeName + TEXT(".weapon.json")));
	return Weapon;
}

bool FMantlePreviewExport::ClientReadsWeapons(const FMantleTargetFeatures& Features)
{
	return Features.Reads(MantleClientFeatures::WeaponDocument);
}

FString FMantlePreviewExport::WeaponsNeedNewerClientMessage()
{
	return TEXT("Your Mantle app is too old to try weapons. Update Mantle in Steam or Epic, then press Test locally again.");
}

FString FMantlePreviewExport::PackagesToJson(const TArray<FMantleLocalScriptPackage>& Packages)
{
	TArray<TSharedPtr<FJsonValue>> Entries;
	for (const FMantleLocalScriptPackage& Package : Packages)
	{
		if (!Package.bHasManifest || Package.Entries.Num() == 0)
		{
			continue;
		}
		const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Package.Name);
		Entry->SetObjectField(TEXT("manifest"), FMantleScriptPackages::ManifestToJsonObject(Package.Manifest));
		Entries.Add(MakeShared<FJsonValueObject>(Entry));
	}
	if (Entries.Num() == 0)
	{
		return FString();
	}
	FString Json;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	FJsonSerializer::Serialize(Entries, Writer);
	return Json;
}

FString FMantlePreviewExport::WeaponsToJson(const TArray<FMantlePreviewBundleWeapon>& Weapons)
{
	FString Json;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	Writer->WriteArrayStart();
	for (const FMantlePreviewBundleWeapon& Weapon : Weapons)
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("file"), Weapon.File);
		TArray<FString> Names;
		Weapon.Assets.GetKeys(Names);
		Names.Sort();
		Writer->WriteObjectStart(TEXT("assets"));
		for (const FString& Name : Names)
		{
			Writer->WriteValue(Name, Weapon.Assets.FindChecked(Name));
		}
		Writer->WriteObjectEnd();
		Writer->WriteArrayStart(TEXT("materials"));
		for (const FString& MaterialId : Weapon.Materials)
		{
			Writer->WriteValue(MaterialId);
		}
		Writer->WriteArrayEnd();
		Writer->WriteValue(TEXT("equip"), Weapon.bEquip);
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();
	Writer->Close();
	return Json;
}

FString FMantlePreviewExport::ReadWeaponMeshName(const FString& DocumentText)
{
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(DocumentText);
	FString Mesh;
	if (FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid())
	{
		Root->TryGetStringField(TEXT("mesh"), Mesh);
	}
	return Mesh;
}

bool FMantlePreviewExport::FindCookTool(const FString& Override, FString& OutTool, FString& OutError)
{
	using namespace MantlePreviewExportPrivate;
	OutTool.Reset();
	FString Candidate = PreviewUnquoted(Override);
	FString From = TEXT("-CookTool");
	if (Candidate.IsEmpty())
	{
		Candidate = PreviewUnquoted(FPlatformMisc::GetEnvironmentVariable(TEXT("MANTLE_COOK_TOOL")));
		From = TEXT("MANTLE_COOK_TOOL");
	}
	if (Candidate.IsEmpty())
	{
		OutError = TEXT("There is no cook tool to cook the preview's assets with. Pass -CookTool=<path to mantle-cook.exe> or set the MANTLE_COOK_TOOL environment variable.");
		return false;
	}
	const FString Full = FPaths::ConvertRelativePathToFull(Candidate);
	if (!IFileManager::Get().FileExists(*Full))
	{
		OutError = FString::Printf(TEXT("The cook tool named by %s does not exist: %s"), *From, *Full);
		return false;
	}
	OutTool = MantlePreviewShared::PlatformPath(Full);
	return true;
}

bool FMantlePreviewExport::Run(UWorld* World, const FMantlePreviewExportOptions& Options, FMantlePreviewExportResult& OutResult)
{
	using namespace MantlePreviewExportPrivate;
	OutResult = FMantlePreviewExportResult();
	FExportState State;
	// #804: the bake caches this export fills are dropped on the way out, unless a publish or another preview is still using them.
	const FMantlePublishCaches::FScope CachesInUse;

	// A dialog the creator can cancel. Under -unattended or a commandlet it only logs, and ShouldCancel stays false.
	FScopedSlowTask Progress(100.f, FText::FromString(TEXT("Exporting the preview")));
	Progress.MakeDialog(/*bShowCancelButton*/ true);
	State.Progress = &Progress;
	const bool bOk = PreviewExportBundle(World, Options, State, OutResult);
	State.Progress = nullptr;

	if (!State.WorkDir.IsEmpty())
	{
		IFileManager::Get().DeleteDirectory(*State.WorkDir, /*RequireExists*/ false, /*Tree*/ true);
	}
	if (!bOk)
	{
		// Only the generation folder this export made.
		if (!State.BundleDir.IsEmpty())
		{
			IFileManager::Get().DeleteDirectory(*State.BundleDir, /*RequireExists*/ false, /*Tree*/ true);
		}
		if (State.bCancelled)
		{
			OutResult.bCancelled = true;
			OutResult.Errors.Reset();
			OutResult.Errors.Add(TEXT("Cancelled."));
		}
		else if (OutResult.Errors.Num() == 0)
		{
			OutResult.Errors.Add(TEXT("The preview export stopped without saying why."));
		}
		OutResult.BundleDirectory.Reset();
		OutResult.ManifestFile.Reset();
		OutResult.CoverageFile.Reset();
		OutResult.bSuccess = false;
		return false;
	}
	OutResult.bSuccess = true;
	return true;
}

namespace MantlePreviewExportPrivate
{
	/** The value of "-Name=value" among Args, unquoted; empty when absent. */
	FString PreviewArgValue(const TArray<FString>& Args, const TCHAR* Prefix)
	{
		for (const FString& Arg : Args)
		{
			if (Arg.StartsWith(Prefix, ESearchCase::IgnoreCase))
			{
				return PreviewUnquoted(Arg.RightChop(FCString::Strlen(Prefix)));
			}
		}
		return FString();
	}

	FAutoConsoleCommandWithWorldAndArgs GMantlePreviewExportCommand(
		TEXT("Mantle.Preview.Export"),
		TEXT("Mantle.Preview.Export [-CookTool=<path to mantle-cook.exe>] [-Out=<folder>] [-Features=<comma list>|all] -- writes a complete offline preview bundle for the open level ")
		TEXT("(validate, convert, cook every asset locally, scripts, UI, avatar), blocking the editor until it ends. ")
		TEXT("Nothing is sent to Mantle. -CookTool defaults to MANTLE_COOK_TOOL; -Out replaces <Project>/Saved/Mantle/Preview as the folder bundles go under. ")
		TEXT("-Features names the gated features the client you will preview in reads (MantleClientFeatures: materialFidelity, terrainGrass, ...): the cook lets their keys through, ")
		TEXT("and an export for a client without materialFidelity or terrainGrass leaves foliage shading and landscape grass out, reported as needing a newer Mantle client. ")
		TEXT("It defaults to all, so a bare export with no client is a dev export for a client as new as this build. ")
		TEXT("Logs MANTLE_PREVIEW_EXPORT_DONE <manifest path> or MANTLE_PREVIEW_EXPORT_FAILED <reason> per problem."),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			FMantlePreviewExportOptions Options;
			Options.CookTool = PreviewArgValue(Args, TEXT("-CookTool="));
			Options.OutputRoot = PreviewArgValue(Args, TEXT("-Out="));
			// #628: a bare export has no client to ask, so it is a dev export: every feature this build reads.
			const bool bHasFeatures = Args.ContainsByPredicate([](const FString& Arg) { return Arg.StartsWith(TEXT("-Features="), ESearchCase::IgnoreCase); });
			Options.Features = bHasFeatures ? FMantleTargetFeatures::Parse(PreviewArgValue(Args, TEXT("-Features="))) : FMantleTargetFeatures::All();

			// The console hands over the world it ran in; from -ExecCmds that can be none, and the level
			// open in the editor is the one to export.
			if ((!World || World->WorldType != EWorldType::Editor) && GEditor)
			{
				World = GEditor->GetEditorWorldContext().World();
			}
			// The converter's cache key follows package dependencies through the asset registry.
			if (IAssetRegistry* Registry = IAssetRegistry::Get())
			{
				if (Registry->IsLoadingAssets())
				{
					UE_LOG(LogMantle, Display, TEXT("MANTLE_PREVIEW_EXPORT waiting for the asset registry"));
					Registry->WaitForCompletion();
				}
			}

			const double Started = FPlatformTime::Seconds();
			FMantlePreviewExportResult Result;
			FMantlePreviewExport::Run(World, Options, Result);
			for (const FString& Warning : Result.Warnings)
			{
				UE_LOG(LogMantle, Display, TEXT("MANTLE_PREVIEW_EXPORT_WARNING %s"), *PreviewOneLine(Warning));
			}
			if (Result.bSuccess)
			{
				UE_LOG(LogMantle, Display, TEXT("MANTLE_PREVIEW_EXPORT_DONE %s assets=%d scripts=%d uiDocuments=%d avatar=%s seconds=%.1f"),
					*Result.ManifestFile, Result.NumAssets, Result.NumScripts, Result.NumUIDocuments, Result.bHasAvatar ? TEXT("yes") : TEXT("no"),
					FPlatformTime::Seconds() - Started);
				return;
			}
			for (const FString& Error : Result.Errors)
			{
				UE_LOG(LogMantle, Error, TEXT("MANTLE_PREVIEW_EXPORT_FAILED %s"), *PreviewOneLine(Error));
			}
		}));
}
