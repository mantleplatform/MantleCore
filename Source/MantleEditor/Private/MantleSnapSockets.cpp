// Copyright Joshua Eger. All Rights Reserved.
#include "MantleSnapSockets.h"

#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshSocket.h"
#include "MantlePlaceLimits.h"

const TCHAR* const MantleSnapSockets::Prefix = TEXT("Snap_");

namespace MantleSnapSocketsPrivate
{
	bool IsWord(const FString& Text)
	{
		if (Text.IsEmpty() || Text.Len() > FMantlePlaceLimits::MaxMeshSocketNameLength)
		{
			return false;
		}
		for (const TCHAR Char : Text)
		{
			if (Char > 127 || (!FChar::IsAlnum(Char) && Char != TEXT('_')))
			{
				return false;
			}
		}
		return true;
	}

	/** Snap_wall_bottom -> wall_bottom; Snap_wall_bottom__left -> wall_bottom. */
	FString KindOf(const FString& SocketName)
	{
		FString Kind = SocketName.Mid(FCString::Strlen(MantleSnapSockets::Prefix));
		const int32 Double = Kind.Find(TEXT("__"));
		if (Double != INDEX_NONE)
		{
			Kind.LeftInline(Double);
		}
		return Kind;
	}
}

void MantleSnapSockets::Convert(TConstArrayView<FMeshSocket> Sockets, TArray<FMantlePlaceSocket>& OutSockets, TArray<FString>& OutProblems)
{
	using namespace MantleSnapSocketsPrivate;
	for (const FMeshSocket& Socket : Sockets)
	{
		const FString Name = Socket.Name.ToString();
		if (!Name.StartsWith(Prefix, ESearchCase::CaseSensitive))
		{
			continue;
		}
		if (OutSockets.Num() >= FMantlePlaceLimits::MaxMeshSockets)
		{
			OutProblems.Add(FString::Printf(TEXT("'%s' is left out: a mesh has at most %d snap sockets"), *Name, FMantlePlaceLimits::MaxMeshSockets));
			continue;
		}
		const FString Kind = KindOf(Name);
		if (!IsWord(Name) || !IsWord(Kind))
		{
			OutProblems.Add(FString::Printf(TEXT("'%s' is left out: a snap socket's name and kind use letters, digits and _ only (up to %d characters)"),
				*Name, FMantlePlaceLimits::MaxMeshSocketNameLength));
			continue;
		}
		if (Socket.Location.ContainsNaN() || Socket.Location.GetAbsMax() > FMantlePlaceLimits::MaxMeshSocketPosition)
		{
			OutProblems.Add(FString::Printf(TEXT("'%s' is left out: it is too far from the mesh's origin"), *Name));
			continue;
		}
		FMantlePlaceSocket Out;
		Out.Name = Name;
		Out.Kind = Kind;
		Out.Position = Socket.Location;
		Out.Rotation = Socket.Rotation.GetNormalized();
		TArray<FString> Words;
		Socket.Tag.ParseIntoArray(Words, TEXT(",; \t"), /*InCullEmpty*/ true);
		bool bBad = false;
		for (const FString& Word : Words)
		{
			if (!IsWord(Word))
			{
				OutProblems.Add(FString::Printf(TEXT("'%s' is left out: its Tag lists '%s', which is not a socket kind (letters, digits and _)"), *Name, *Word));
				bBad = true;
				break;
			}
			Out.Accepts.AddUnique(Word);
		}
		if (bBad)
		{
			continue;
		}
		if (Out.Accepts.Num() > FMantlePlaceLimits::MaxMeshSocketAccepts)
		{
			OutProblems.Add(FString::Printf(TEXT("'%s' is left out: its Tag lists more than %d kinds"), *Name, FMantlePlaceLimits::MaxMeshSocketAccepts));
			continue;
		}
		OutSockets.Add(MoveTemp(Out));
	}
}

void MantleSnapSockets::Read(const UStaticMesh& Mesh, TArray<FMantlePlaceSocket>& OutSockets, TArray<FString>& OutProblems)
{
	TArray<FMeshSocket> Sockets;
	for (const UStaticMeshSocket* Socket : Mesh.Sockets)
	{
		if (Socket)
		{
			FMeshSocket& Added = Sockets.AddDefaulted_GetRef();
			Added.Name = Socket->SocketName;
			Added.Tag = Socket->Tag;
			Added.Location = Socket->RelativeLocation;
			Added.Rotation = Socket->RelativeRotation;
		}
	}
	Convert(Sockets, OutSockets, OutProblems);
}
