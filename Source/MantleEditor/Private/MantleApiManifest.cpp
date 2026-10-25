// Copyright Joshua Eger. All Rights Reserved.
#include "MantleApiManifest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogMantleApiManifest, Log, All);

namespace MantleApiManifestPrivate
{
	bool ReadSide(const FJsonObject& Object, const TCHAR* Field, EMantleSide& Out, FString& OutError)
	{
		FString Text;
		if (!Object.TryGetStringField(Field, Text))
		{
			OutError = FString::Printf(TEXT("missing \"%s\""), Field);
			return false;
		}
		if (Text == TEXT("server")) { Out = EMantleSide::Server; return true; }
		if (Text == TEXT("client")) { Out = EMantleSide::Client; return true; }
		if (Text == TEXT("both")) { Out = EMantleSide::Both; return true; }
		if (Text == TEXT("none")) { Out = EMantleSide::None; return true; }
		OutError = FString::Printf(TEXT("\"%s\" is \"%s\", not server, client, both or none"), Field, *Text);
		return false;
	}

	/** The common member fields every method, property and signal carries. */
	template <typename TMember>
	void ReadMemberCommon(const FJsonObject& Object, TMember& Out)
	{
		Object.TryGetStringField(TEXT("name"), Out.Name);
		Object.TryGetStringField(TEXT("doc"), Out.Doc);
		Object.TryGetBoolField(TEXT("deprecated"), Out.bDeprecated);
		Object.TryGetStringField(TEXT("deprecatedReason"), Out.DeprecatedReason);
		Object.TryGetNumberField(TEXT("introducedMinor"), Out.IntroducedMinor);
	}

	const TArray<TSharedPtr<FJsonValue>>* Array(const FJsonObject& Object, const TCHAR* Field)
	{
		const TArray<TSharedPtr<FJsonValue>>* Out = nullptr;
		return Object.TryGetArrayField(Field, Out) ? Out : nullptr;
	}
}

int32 FMantleApiManifestClass::MemberIntroducedMinor(const FString& Member) const
{
	int32 MemberMinor = INDEX_NONE;
	auto Named = [&Member](const auto& X) { return X.Name.Equals(Member, ESearchCase::CaseSensitive); };
	if (const FMantleApiManifestMethod* M = Methods.FindByPredicate(Named))
	{
		MemberMinor = M->IntroducedMinor;
	}
	else if (const FMantleApiManifestProperty* P = Properties.FindByPredicate(Named))
	{
		MemberMinor = P->IntroducedMinor;
	}
	else if (const FMantleApiManifestSignal* S = Signals.FindByPredicate(Named))
	{
		MemberMinor = S->IntroducedMinor;
	}
	return MemberMinor == INDEX_NONE ? INDEX_NONE : FMath::Max(MemberMinor, IntroducedMinor);
}

bool FMantleApiManifest::Parse(const FString& Json, FMantleApiManifest& Out, FString& OutError)
{
	using namespace MantleApiManifestPrivate;

	Out = FMantleApiManifest();
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid())
	{
		OutError = TEXT("not a JSON object");
		return false;
	}
	if (!Root->TryGetNumberField(TEXT("major"), Out.Major) || !Root->TryGetNumberField(TEXT("minor"), Out.Minor) || Out.Major <= 0)
	{
		OutError = TEXT("missing the API's major and minor");
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Classes = Array(*Root, TEXT("classes"));
	if (!Classes)
	{
		OutError = TEXT("missing \"classes\"");
		return false;
	}

	for (const TSharedPtr<FJsonValue>& ClassValue : *Classes)
	{
		const TSharedPtr<FJsonObject>* ClassObject = nullptr;
		if (!ClassValue.IsValid() || !ClassValue->TryGetObject(ClassObject))
		{
			OutError = TEXT("a class is not an object");
			return false;
		}
		const FJsonObject& C = **ClassObject;
		FMantleApiManifestClass& Class = Out.Classes.AddDefaulted_GetRef();
		C.TryGetStringField(TEXT("name"), Class.Name);
		C.TryGetStringField(TEXT("doc"), Class.Doc);
		C.TryGetBoolField(TEXT("service"), Class.bService);
		C.TryGetNumberField(TEXT("introducedMinor"), Class.IntroducedMinor);
		if (Class.Name.IsEmpty())
		{
			OutError = TEXT("a class has no name");
			return false;
		}

		if (const TArray<TSharedPtr<FJsonValue>>* Methods = Array(C, TEXT("methods")))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Methods)
			{
				const FJsonObject& M = *Value->AsObject();
				FMantleApiManifestMethod& Method = Class.Methods.AddDefaulted_GetRef();
				ReadMemberCommon(M, Method);
				M.TryGetBoolField(TEXT("variadic"), Method.bVariadic);
				M.TryGetStringField(TEXT("returns"), Method.Returns);
				M.TryGetBoolField(TEXT("kit"), Method.bKit);
				if (!ReadSide(M, TEXT("side"), Method.Side, OutError))
				{
					OutError = Class.Name + TEXT(".") + Method.Name + TEXT(": ") + OutError;
					return false;
				}
				if (const TArray<TSharedPtr<FJsonValue>>* Params = Array(M, TEXT("params")))
				{
					for (const TSharedPtr<FJsonValue>& ParamValue : *Params)
					{
						const FJsonObject& P = *ParamValue->AsObject();
						FMantleApiManifestParam& Param = Method.Params.AddDefaulted_GetRef();
						P.TryGetStringField(TEXT("name"), Param.Name);
						P.TryGetStringField(TEXT("type"), Param.Type);
						P.TryGetBoolField(TEXT("optional"), Param.bOptional);
					}
				}
			}
		}
		if (const TArray<TSharedPtr<FJsonValue>>* Properties = Array(C, TEXT("properties")))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Properties)
			{
				const FJsonObject& P = *Value->AsObject();
				FMantleApiManifestProperty& Property = Class.Properties.AddDefaulted_GetRef();
				ReadMemberCommon(P, Property);
				P.TryGetStringField(TEXT("type"), Property.Type);
				P.TryGetBoolField(TEXT("settable"), Property.bSettable);
				if (!ReadSide(P, TEXT("getSide"), Property.GetSide, OutError) || !ReadSide(P, TEXT("setSide"), Property.SetSide, OutError))
				{
					OutError = Class.Name + TEXT(".") + Property.Name + TEXT(": ") + OutError;
					return false;
				}
			}
		}
		if (const TArray<TSharedPtr<FJsonValue>>* Signals = Array(C, TEXT("signals")))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Signals)
			{
				const FJsonObject& S = *Value->AsObject();
				FMantleApiManifestSignal& Signal = Class.Signals.AddDefaulted_GetRef();
				ReadMemberCommon(S, Signal);
				S.TryGetStringField(TEXT("args"), Signal.Args);
				if (!ReadSide(S, TEXT("side"), Signal.Side, OutError))
				{
					OutError = Class.Name + TEXT(".") + Signal.Name + TEXT(": ") + OutError;
					return false;
				}
			}
		}
	}
	return true;
}

FString FMantleApiManifest::ResourceDirectory()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MantleCore"));
	if (!Plugin.IsValid())
	{
		return FString();
	}
	return FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("ScriptApi"));
}

const FMantleApiManifest& FMantleApiManifest::Get()
{
	static const FMantleApiManifest Manifest = []()
	{
		FMantleApiManifest Loaded;
		const FString Path = FPaths::Combine(ResourceDirectory(), TEXT("mantle_api.json"));
		FString Json;
		FString Error;
		if (!FFileHelper::LoadFileToString(Json, *Path))
		{
			UE_LOG(LogMantleApiManifest, Error, TEXT("Could not read the script API manifest %s."), *Path);
		}
		else if (!Parse(Json, Loaded, Error))
		{
			UE_LOG(LogMantleApiManifest, Error, TEXT("The script API manifest %s is broken: %s."), *Path, *Error);
			Loaded = FMantleApiManifest();
		}
		return Loaded;
	}();
	return Manifest;
}

const FMantleApiManifestClass* FMantleApiManifest::FindClass(const FString& Name) const
{
	// Ignoring case, as the game's registry looks classes up (FMantleApi::FindClass's FString-keyed map).
	return Classes.FindByPredicate([&Name](const FMantleApiManifestClass& Class) { return Class.Name.Equals(Name, ESearchCase::IgnoreCase); });
}
