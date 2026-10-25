// Copyright Joshua Eger. All Rights Reserved.
#include "Graph/MantleGraphTypes.h"

#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	using FGraphJsonWriter = TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>;
	using FGraphJsonWriterFactory = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>;

	void MantleGraphWriteJsonValue(const TSharedRef<FGraphJsonWriter>& Writer, const TSharedPtr<FJsonValue>& Value, const FString* Key = nullptr)
	{
		const EJson Type = Value.IsValid() ? Value->Type : EJson::Null;
		switch (Type)
		{
		case EJson::String:
			if (Key) Writer->WriteValue(*Key, Value->AsString()); else Writer->WriteValue(Value->AsString());
			break;
		case EJson::Number:
			if (Key) Writer->WriteValue(*Key, Value->AsNumber()); else Writer->WriteValue(Value->AsNumber());
			break;
		case EJson::Boolean:
			if (Key) Writer->WriteValue(*Key, Value->AsBool()); else Writer->WriteValue(Value->AsBool());
			break;
		case EJson::Array:
			if (Key) Writer->WriteArrayStart(*Key); else Writer->WriteArrayStart();
			for (const TSharedPtr<FJsonValue>& Child : Value->AsArray())
			{
				MantleGraphWriteJsonValue(Writer, Child);
			}
			Writer->WriteArrayEnd();
			break;
		case EJson::Object:
		{
			if (Key) Writer->WriteObjectStart(*Key); else Writer->WriteObjectStart();
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			if (Object.IsValid())
			{
				TArray<TPair<FString, TSharedPtr<FJsonValue>>> Members;
				for (const auto& Pair : Object->Values)
				{
					Members.Emplace(FString(Pair.Key.ToView()), Pair.Value);
				}
				Members.Sort([](const auto& A, const auto& B) { return A.Key < B.Key; });
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Member : Members)
				{
					MantleGraphWriteJsonValue(Writer, Member.Value, &Member.Key);
				}
			}
			Writer->WriteObjectEnd();
			break;
		}
		case EJson::Null:
		case EJson::None:
		default:
			if (Key) Writer->WriteNull(*Key); else Writer->WriteNull();
			break;
		}
	}

	void MantleGraphWriteParams(const TSharedRef<FGraphJsonWriter>& Writer, const TCHAR* Key, const TArray<FMantleGraphParam>& Params)
	{
		if (Params.Num() == 0) return;
		Writer->WriteArrayStart(Key);
		for (const FMantleGraphParam& Param : Params)
		{
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("name"), Param.Name);
			Writer->WriteValue(TEXT("type"), Param.Type);
			Writer->WriteObjectEnd();
		}
		Writer->WriteArrayEnd();
	}

	/** Collects parse errors with a JSON-pointer-style path, matching FMantlePlaceParser's
	 *  convention (MantleRuntime/Private/MantlePlaceParser.cpp) so both read the same way in a
	 *  log. Parsing stops at the first error: an ill-shaped graph can't be walked further. */
	struct FMantleGraphParseState
	{
		FString Error;
		bool Failed() const { return !Error.IsEmpty(); }
		void Fail(const FString& Path, const FString& Message)
		{
			if (Error.IsEmpty())
			{
				Error = FString::Printf(TEXT("%s: %s"), *Path, *Message);
			}
		}
	};

	FString MantleGraphJoinPath(const FString& Path, const FString& Key)
	{
		return Path + TEXT("/") + Key;
	}

	FString MantleGraphIndexPath(const FString& Path, int32 Index)
	{
		return Path + TEXT("/") + FString::FromInt(Index);
	}

	bool MantleGraphRequireString(FMantleGraphParseState& St, const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, const FString& Path, FString& Out)
	{
		if (!Obj->TryGetStringField(Key, Out) || Out.IsEmpty())
		{
			St.Fail(MantleGraphJoinPath(Path, Key), TEXT("required string is missing or empty"));
			return false;
		}
		return true;
	}

	bool MantleGraphParseParam(FMantleGraphParseState& St, const TSharedPtr<FJsonValue>& Value, const FString& Path, FMantleGraphParam& Out)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(Obj))
		{
			St.Fail(Path, TEXT("param must be an object with \"name\" and \"type\""));
			return false;
		}
		return MantleGraphRequireString(St, *Obj, TEXT("name"), Path, Out.Name)
			&& MantleGraphRequireString(St, *Obj, TEXT("type"), Path, Out.Type);
	}

	bool MantleGraphParseParamArray(FMantleGraphParseState& St, const TSharedPtr<FJsonObject>& Obj, const TCHAR* Key, const FString& Path, TArray<FMantleGraphParam>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Obj->TryGetArrayField(Key, Arr))
		{
			return true; // optional; an empty param list is the common case (custom events, no-arg functions)
		}
		for (int32 i = 0; i < Arr->Num(); ++i)
		{
			FMantleGraphParam Param;
			if (!MantleGraphParseParam(St, (*Arr)[i], MantleGraphIndexPath(MantleGraphJoinPath(Path, Key), i), Param))
			{
				return false;
			}
			Out.Add(MoveTemp(Param));
		}
		return true;
	}

	bool MantleGraphParseVariables(FMantleGraphParseState& St, const TSharedPtr<FJsonObject>& Root, TArray<FMantleGraphVariable>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Root->TryGetArrayField(TEXT("variables"), Arr))
		{
			return true;
		}
		for (int32 i = 0; i < Arr->Num(); ++i)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			const FString Path = MantleGraphIndexPath(TEXT("/variables"), i);
			if (!(*Arr)[i].IsValid() || !(*Arr)[i]->TryGetObject(Obj))
			{
				St.Fail(Path, TEXT("variable must be an object"));
				return false;
			}
			FMantleGraphVariable Var;
			if (!MantleGraphRequireString(St, *Obj, TEXT("name"), Path, Var.Name)
				|| !MantleGraphRequireString(St, *Obj, TEXT("type"), Path, Var.Type))
			{
				return false;
			}
			Var.Default = (*Obj)->TryGetField(TEXT("default"));
			Out.Add(MoveTemp(Var));
		}
		return true;
	}

	bool MantleGraphParseCustomEvents(FMantleGraphParseState& St, const TSharedPtr<FJsonObject>& Root, TArray<FMantleGraphCustomEvent>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Root->TryGetArrayField(TEXT("customEvents"), Arr))
		{
			return true;
		}
		for (int32 i = 0; i < Arr->Num(); ++i)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			const FString Path = MantleGraphIndexPath(TEXT("/customEvents"), i);
			if (!(*Arr)[i].IsValid() || !(*Arr)[i]->TryGetObject(Obj))
			{
				St.Fail(Path, TEXT("custom event must be an object"));
				return false;
			}
			FMantleGraphCustomEvent Event;
			if (!MantleGraphRequireString(St, *Obj, TEXT("name"), Path, Event.Name)
				|| !MantleGraphParseParamArray(St, *Obj, TEXT("params"), Path, Event.Params))
			{
				return false;
			}
			Out.Add(MoveTemp(Event));
		}
		return true;
	}

	bool MantleGraphParseFunctions(FMantleGraphParseState& St, const TSharedPtr<FJsonObject>& Root, TArray<FMantleGraphFunction>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Root->TryGetArrayField(TEXT("functions"), Arr))
		{
			return true;
		}
		for (int32 i = 0; i < Arr->Num(); ++i)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			const FString Path = MantleGraphIndexPath(TEXT("/functions"), i);
			if (!(*Arr)[i].IsValid() || !(*Arr)[i]->TryGetObject(Obj))
			{
				St.Fail(Path, TEXT("function must be an object"));
				return false;
			}
			FMantleGraphFunction Fn;
			if (!MantleGraphRequireString(St, *Obj, TEXT("name"), Path, Fn.Name)
				|| !MantleGraphParseParamArray(St, *Obj, TEXT("params"), Path, Fn.Params)
				|| !MantleGraphParseParamArray(St, *Obj, TEXT("returns"), Path, Fn.Returns))
			{
				return false;
			}
			Out.Add(MoveTemp(Fn));
		}
		return true;
	}

	bool MantleGraphParseNodes(FMantleGraphParseState& St, const TSharedPtr<FJsonObject>& Root, TArray<FMantleGraphNode>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Root->TryGetArrayField(TEXT("nodes"), Arr) || Arr->Num() == 0)
		{
			St.Fail(TEXT("/nodes"), TEXT("required non-empty array is missing"));
			return false;
		}
		TSet<FString> SeenIds;
		for (int32 i = 0; i < Arr->Num(); ++i)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			const FString Path = MantleGraphIndexPath(TEXT("/nodes"), i);
			if (!(*Arr)[i].IsValid() || !(*Arr)[i]->TryGetObject(Obj))
			{
				St.Fail(Path, TEXT("node must be an object"));
				return false;
			}
			FMantleGraphNode Node;
			if (!MantleGraphRequireString(St, *Obj, TEXT("id"), Path, Node.Id)
				|| !MantleGraphRequireString(St, *Obj, TEXT("kind"), Path, Node.Kind))
			{
				return false;
			}
			if (SeenIds.Contains(Node.Id))
			{
				St.Fail(Path, FString::Printf(TEXT("duplicate node id '%s'"), *Node.Id));
				return false;
			}
			SeenIds.Add(Node.Id);
			Node.Fields = *Obj;
			Out.Add(MoveTemp(Node));
		}
		return true;
	}

	bool MantleGraphParseExecWires(FMantleGraphParseState& St, const TSharedPtr<FJsonObject>& Root, TArray<FMantleGraphExecWire>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Root->TryGetArrayField(TEXT("exec"), Arr))
		{
			return true; // a single-node graph (e.g. one "start" with no body) is degenerate but not ill-formed
		}
		for (int32 i = 0; i < Arr->Num(); ++i)
		{
			const FString Path = MantleGraphIndexPath(TEXT("/exec"), i);
			const TSharedPtr<FJsonValue>& Entry = (*Arr)[i];
			FMantleGraphExecWire Wire;
			const TArray<TSharedPtr<FJsonValue>>* Pair = nullptr;
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (Entry.IsValid() && Entry->TryGetArray(Pair))
			{
				// Shorthand ["from", "to"] for the common single-"next"-port case.
				if (Pair->Num() != 2 || !(*Pair)[0]->TryGetString(Wire.From) || !(*Pair)[1]->TryGetString(Wire.To))
				{
					St.Fail(Path, TEXT("exec pair must be [\"from\", \"to\"]"));
					return false;
				}
			}
			else if (Entry.IsValid() && Entry->TryGetObject(Obj))
			{
				if (!MantleGraphRequireString(St, *Obj, TEXT("from"), Path, Wire.From)
					|| !MantleGraphRequireString(St, *Obj, TEXT("to"), Path, Wire.To))
				{
					return false;
				}
				FString Port;
				if ((*Obj)->TryGetStringField(TEXT("port"), Port) && !Port.IsEmpty())
				{
					Wire.Port = Port;
				}
			}
			else
			{
				St.Fail(Path, TEXT("exec entry must be [\"from\", \"to\"] or {\"from\", \"port\", \"to\"}"));
				return false;
			}
			Out.Add(MoveTemp(Wire));
		}
		return true;
	}
}

const FMantleGraphNode* FMantleGraph::FindNode(const FString& Id) const
{
	return Nodes.FindByPredicate([&](const FMantleGraphNode& N) { return N.Id == Id; });
}

const FMantleGraphVariable* FMantleGraph::FindVariable(const FString& InName) const
{
	return Variables.FindByPredicate([&](const FMantleGraphVariable& V) { return V.Name == InName; });
}

const FMantleGraphCustomEvent* FMantleGraph::FindCustomEvent(const FString& InName) const
{
	return CustomEvents.FindByPredicate([&](const FMantleGraphCustomEvent& E) { return E.Name == InName; });
}

const FMantleGraphFunction* FMantleGraph::FindFunction(const FString& InName) const
{
	return Functions.FindByPredicate([&](const FMantleGraphFunction& F) { return F.Name == InName; });
}

bool FMantleGraph::ParseFromJson(const FString& JsonText, FMantleGraph& OutGraph, FString& OutError)
{
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = TEXT("/: invalid JSON");
		return false;
	}

	FMantleGraphParseState St;
	FMantleGraph Graph;

	MantleGraphRequireString(St, Root, TEXT("name"), TEXT(""), Graph.Name);
	FString SideText;
	if (!St.Failed() && MantleGraphRequireString(St, Root, TEXT("side"), TEXT(""), SideText))
	{
		if (SideText == TEXT("server")) Graph.Side = EMantleSide::Server;
		else if (SideText == TEXT("client")) Graph.Side = EMantleSide::Client;
		else St.Fail(TEXT("/side"), TEXT("must be \"server\" or \"client\""));
	}
	if (!St.Failed()) MantleGraphParseVariables(St, Root, Graph.Variables);
	if (!St.Failed()) MantleGraphParseCustomEvents(St, Root, Graph.CustomEvents);
	if (!St.Failed()) MantleGraphParseFunctions(St, Root, Graph.Functions);
	if (!St.Failed()) MantleGraphParseNodes(St, Root, Graph.Nodes);
	if (!St.Failed()) MantleGraphParseExecWires(St, Root, Graph.Exec);

	if (St.Failed())
	{
		OutError = St.Error;
		return false;
	}
	OutGraph = MoveTemp(Graph);
	return true;
}

bool FMantleGraph::ParseFromFile(const FString& FilePath, FMantleGraph& OutGraph, FString& OutError)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *FilePath))
	{
		OutError = FString::Printf(TEXT("could not read '%s'"), *FilePath);
		return false;
	}
	return ParseFromJson(Text, OutGraph, OutError);
}

FString FMantleGraph::ToJson(const FMantleGraph& Graph)
{
	FString Output;
	const TSharedRef<FGraphJsonWriter> Writer = FGraphJsonWriterFactory::Create(&Output);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("name"), Graph.Name);
	Writer->WriteValue(TEXT("side"), Graph.Side == EMantleSide::Client ? TEXT("client") : TEXT("server"));

	if (Graph.Variables.Num() > 0)
	{
		Writer->WriteArrayStart(TEXT("variables"));
		for (const FMantleGraphVariable& Variable : Graph.Variables)
		{
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("name"), Variable.Name);
			Writer->WriteValue(TEXT("type"), Variable.Type);
			if (Variable.Default.IsValid())
			{
				const FString DefaultKey(TEXT("default"));
				MantleGraphWriteJsonValue(Writer, Variable.Default, &DefaultKey);
			}
			Writer->WriteObjectEnd();
		}
		Writer->WriteArrayEnd();
	}
	if (Graph.CustomEvents.Num() > 0)
	{
		Writer->WriteArrayStart(TEXT("customEvents"));
		for (const FMantleGraphCustomEvent& Event : Graph.CustomEvents)
		{
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("name"), Event.Name);
			MantleGraphWriteParams(Writer, TEXT("params"), Event.Params);
			Writer->WriteObjectEnd();
		}
		Writer->WriteArrayEnd();
	}
	if (Graph.Functions.Num() > 0)
	{
		Writer->WriteArrayStart(TEXT("functions"));
		for (const FMantleGraphFunction& Function : Graph.Functions)
		{
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("name"), Function.Name);
			MantleGraphWriteParams(Writer, TEXT("params"), Function.Params);
			MantleGraphWriteParams(Writer, TEXT("returns"), Function.Returns);
			Writer->WriteObjectEnd();
		}
		Writer->WriteArrayEnd();
	}

	Writer->WriteArrayStart(TEXT("nodes"));
	for (const FMantleGraphNode& Node : Graph.Nodes)
	{
		MantleGraphWriteJsonValue(Writer, MakeShared<FJsonValueObject>(Node.Fields));
	}
	Writer->WriteArrayEnd();

	if (Graph.Exec.Num() > 0)
	{
		Writer->WriteArrayStart(TEXT("exec"));
		for (const FMantleGraphExecWire& Wire : Graph.Exec)
		{
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("from"), Wire.From);
			if (!Wire.Port.IsEmpty()) Writer->WriteValue(TEXT("port"), Wire.Port);
			Writer->WriteValue(TEXT("to"), Wire.To);
			Writer->WriteObjectEnd();
		}
		Writer->WriteArrayEnd();
	}
	Writer->WriteObjectEnd();
	Writer->Close();
	return Output;
}
