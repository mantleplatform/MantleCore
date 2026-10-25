// Copyright Joshua Eger. All Rights Reserved.
#include "Graph/MantleGraphCatalogue.h"

#include "MantleApiManifest.h"

namespace
{
	/** Splits "(a: T, b: U)" (or the same with a trailing " -> Returns" already stripped) into
	 *  top-level comma-separated entries. There is no nesting to worry about in a parameter list
	 *  built from FMantleApiManifestParam::Type strings -- types like "{Entity}" or "(Entity) -> ()" only
	 *  ever appear as a single parameter's *own* type, never split further by this pass. */
	TArray<FString> MantleGraphSplitTopLevel(const FString& Inner)
	{
		TArray<FString> Parts;
		int32 Depth = 0;
		int32 Start = 0;
		for (int32 i = 0; i < Inner.Len(); ++i)
		{
			const TCHAR C = Inner[i];
			if (C == TEXT('(') || C == TEXT('{')) ++Depth;
			else if (C == TEXT(')') || C == TEXT('}')) --Depth;
			else if (C == TEXT(',') && Depth == 0)
			{
				Parts.Add(Inner.Mid(Start, i - Start));
				Start = i + 1;
			}
		}
		if (Start < Inner.Len())
		{
			Parts.Add(Inner.Mid(Start));
		}
		return Parts;
	}
}

bool FMantleGraphCatalogue::IsHandlerType(const FString& ParamType)
{
	return ParamType.TrimStartAndEnd().StartsWith(TEXT("("));
}

TArray<FMantleGraphPinDef> FMantleGraphCatalogue::ParseHandlerSignature(const FString& Signature)
{
	TArray<FMantleGraphPinDef> Pins;
	FString Sig = Signature.TrimStartAndEnd();
	FString Discard;
	Sig.Split(TEXT("->"), &Sig, &Discard);
	Sig = Sig.TrimStartAndEnd();
	if (!Sig.StartsWith(TEXT("(")) || !Sig.EndsWith(TEXT(")")))
	{
		return Pins;
	}
	const FString Inner = Sig.Mid(1, Sig.Len() - 2).TrimStartAndEnd();
	if (Inner.IsEmpty())
	{
		return Pins;
	}
	for (const FString& RawEntry : MantleGraphSplitTopLevel(Inner))
	{
		const FString Entry = RawEntry.TrimStartAndEnd();
		FString Name, Type;
		if (!Entry.Split(TEXT(":"), &Name, &Type))
		{
			// Not a "name: Type" entry (e.g. bare "...any" for a fully variadic handler). The
			// graph must declare its own output names for this node; the catalogue can't.
			continue;
		}
		FMantleGraphPinDef Pin;
		Pin.Name = Name.TrimStartAndEnd();
		Type = Type.TrimStartAndEnd();
		Pin.bOptional = Type.EndsWith(TEXT("?"));
		Pin.Type = Pin.bOptional ? Type.LeftChop(1) : Type;
		Pins.Add(MoveTemp(Pin));
	}
	return Pins;
}

namespace
{
	FMantleGraphPinDef MantleGraphMakeParamPin(const FMantleApiManifestParam& Param)
	{
		FMantleGraphPinDef Pin;
		Pin.Name = Param.Name;
		Pin.bOptional = Param.bOptional;
		Pin.Type = Param.Type;
		if (Pin.Type.EndsWith(TEXT("?")))
		{
			Pin.Type = Pin.Type.LeftChop(1);
		}
		return Pin;
	}

	void MantleGraphBuildFromClass(const FMantleApiManifestClass& Class, TArray<FMantleGraphNodeDef>& Out)
	{
		for (const FMantleApiManifestSignal& Signal : Class.Signals)
		{
			FMantleGraphNodeDef Def;
			Def.Category = EMantleGraphNodeCategory::Event;
			Def.ClassName = Class.Name;
			Def.MemberName = Signal.Name;
			Def.Dotted = Class.Name + TEXT(".") + Signal.Name;
			Def.bService = Class.bService;
			Def.bIsSignal = true;
			Def.Side = Signal.Side;
			Def.Outputs = FMantleGraphCatalogue::ParseHandlerSignature(Signal.Args);
			Def.Doc = Signal.Doc;
			Out.Add(MoveTemp(Def));
		}
		for (const FMantleApiManifestMethod& Method : Class.Methods)
		{
			const bool bIsHandlerShaped = Method.Params.Num() > 0 && FMantleGraphCatalogue::IsHandlerType(Method.Params.Last().Type);
			FMantleGraphNodeDef Def;
			Def.ClassName = Class.Name;
			Def.MemberName = Method.Name;
			Def.Dotted = Class.Name + TEXT(".") + Method.Name;
			Def.bService = Class.bService;
			Def.Side = Method.Side;
			Def.Doc = Method.Doc;
			if (bIsHandlerShaped)
			{
				Def.Category = EMantleGraphNodeCategory::Event;
				for (int32 i = 0; i < Method.Params.Num() - 1; ++i)
				{
					Def.Inputs.Add(MantleGraphMakeParamPin(Method.Params[i]));
				}
				Def.Outputs = FMantleGraphCatalogue::ParseHandlerSignature(Method.Params.Last().Type);
			}
			else
			{
				Def.Category = EMantleGraphNodeCategory::Call;
				Def.bVariadic = Method.bVariadic;
				for (const FMantleApiManifestParam& Param : Method.Params)
				{
					Def.Inputs.Add(MantleGraphMakeParamPin(Param));
				}
				if (Method.Returns != TEXT("()") && !Method.Returns.IsEmpty())
				{
					Def.Outputs.Add(FMantleGraphPinDef{TEXT("value"), Method.Returns, false});
				}
			}
			Out.Add(MoveTemp(Def));
		}
		for (const FMantleApiManifestProperty& Property : Class.Properties)
		{
			FMantleGraphNodeDef Get;
			Get.Category = EMantleGraphNodeCategory::Get;
			Get.ClassName = Class.Name;
			Get.MemberName = Property.Name;
			Get.Dotted = Class.Name + TEXT(".") + Property.Name;
			Get.bService = Class.bService;
			Get.Side = Property.GetSide;
			Get.Doc = Property.Doc;
			Get.Outputs.Add(FMantleGraphPinDef{TEXT("value"), Property.Type, false});
			Out.Add(MoveTemp(Get));

			if (Property.bSettable)
			{
				FMantleGraphNodeDef Set;
				Set.Category = EMantleGraphNodeCategory::Set;
				Set.ClassName = Class.Name;
				Set.MemberName = Property.Name;
				Set.Dotted = Class.Name + TEXT(".") + Property.Name;
				Set.bService = Class.bService;
				Set.Side = Property.SetSide;
				Set.Doc = Property.Doc;
				Set.Inputs.Add(FMantleGraphPinDef{TEXT("value"), Property.Type, false});
				Out.Add(MoveTemp(Set));
			}
		}
	}
}

const TArray<FMantleGraphNodeDef>& FMantleGraphCatalogue::All()
{
	static TArray<FMantleGraphNodeDef> Cached;
	static bool bBuilt = false;
	if (!bBuilt)
	{
		bBuilt = true;
		for (const FMantleApiManifestClass& Class : FMantleApiManifest::Get().Classes)
		{
			MantleGraphBuildFromClass(Class, Cached);
		}
	}
	return Cached;
}

const FMantleGraphNodeDef* FMantleGraphCatalogue::FindByDotted(const FString& Dotted, EMantleGraphNodeCategory Category, FString& OutError)
{
	FString ClassName, MemberName;
	if (!Dotted.Split(TEXT("."), &ClassName, &MemberName) || ClassName.IsEmpty() || MemberName.IsEmpty())
	{
		OutError = FString::Printf(TEXT("'%s' is not a valid \"Class.Member\" name"), *Dotted);
		return nullptr;
	}

	const FMantleGraphNodeDef* AnyMatch = nullptr;
	for (const FMantleGraphNodeDef& Def : All())
	{
		if (Def.ClassName != ClassName || Def.MemberName != MemberName)
		{
			continue;
		}
		if (Def.Category == Category)
		{
			return &Def;
		}
		AnyMatch = &Def;
	}

	if (!AnyMatch)
	{
		OutError = FMantleApiManifest::Get().FindClass(ClassName)
			? FString::Printf(TEXT("'%s' is not a member of %s"), *MemberName, *ClassName)
			: FString::Printf(TEXT("unknown class '%s' in '%s'"), *ClassName, *Dotted);
		return nullptr;
	}

	auto CategoryLabel = [](EMantleGraphNodeCategory C)
	{
		switch (C)
		{
		case EMantleGraphNodeCategory::Event: return TEXT("an Event");
		case EMantleGraphNodeCategory::Call: return TEXT("a Call");
		case EMantleGraphNodeCategory::Get: return TEXT("a Get");
		default: return TEXT("a Set");
		}
	};
	OutError = FString::Printf(TEXT("'%s' is %s member, not %s"), *Dotted, CategoryLabel(AnyMatch->Category), CategoryLabel(Category));
	if (AnyMatch->Category == EMantleGraphNodeCategory::Get && Category == EMantleGraphNodeCategory::Set)
	{
		OutError = FString::Printf(TEXT("'%s' is read-only"), *Dotted);
	}
	return nullptr;
}
