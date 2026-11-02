// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "MantlePlaceTypes.h"

class UStaticMesh;

/**
 * Placing mode's snap sockets, authored on a mesh (mantleplatform/mantle#184). A creator adds ordinary Unreal sockets to a StaticMesh,
 * named Snap_<kind> (a second one of the same kind: Snap_<kind>__<anything>), and puts the kinds the socket may join in the socket's
 * Tag, separated by commas or spaces ("floor_edge, wall_top"). Publishing writes them into the place file beside the mesh (assets[].sockets),
 * so the server never loads the mesh to know them. The socket's place and rotation are its relative location and rotation (the scale is
 * ignored); it faces along its forward (+X) axis. Other sockets on the mesh are left alone.
 */
namespace MantleSnapSockets
{
	/** The prefix that marks a socket as a snap socket. */
	extern const TCHAR* const Prefix;

	/** A mesh socket as the editor holds it. */
	struct FMeshSocket
	{
		FName Name;
		FString Tag;
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
	};

	/**
	 * The snap sockets among Sockets, in the place file's form. A socket that cannot be written (a name with a character the file does not
	 * allow, a position too far from the mesh, more than FMantlePlaceLimits::MaxMeshSockets) is left out and explained in OutProblems.
	 */
	void Convert(TConstArrayView<FMeshSocket> Sockets, TArray<FMantlePlaceSocket>& OutSockets, TArray<FString>& OutProblems);

	/** The snap sockets of a mesh asset. */
	void Read(const UStaticMesh& Mesh, TArray<FMantlePlaceSocket>& OutSockets, TArray<FString>& OutProblems);
}
