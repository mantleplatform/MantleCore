// Copyright Joshua Eger. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"

class UBodySetup;
class UStaticMesh;

/**
 * A mirrored static mesh instance becomes a mirrored mesh at export (#628). A place carries scales above zero
 * on every axis, but a level mirrors a mesh with a negative scale (a door hung the other way, a wall's
 * opposite side); the exporter bakes a transient copy of the mesh, mirrored on the negative axes, and places
 * that at the positive scale instead.
 *
 * The copy is made as the spline mesh bake makes its meshes (FMantleSplineMeshBake): a transient static mesh
 * built from a mesh description, never written into the creator's Content, named after its source, and kept
 * alive by this class's own cache so that Gather, Check and Export all see the same object (and the same mesh
 * key) for as long as the source is unchanged.
 *
 * What the copy keeps: every LOD the source's render data can be read for (and its screen sizes), the material
 * slots (same indices, so a component's material overrides still line up), every UV set (the lightmap UVs
 * included) and vertex colours, tangents with their sign, and the simple collision (each shape mirrored;
 * complex-as-simple where the source has none). Triangles are turned over when an odd number of axes is
 * mirrored, so they still face out.
 */
class MANTLEEDITOR_API FMantleMirrorBake
{
public:
	/** Whether Scale mirrors a mesh: any axis below zero. */
	static bool HasNegativeAxis(const FVector& Scale);

	/** Whether a mirror by Scale turns triangles inside out unless they are reversed: an odd number of negative axes. */
	static bool FlipsWinding(const FVector& Scale);

	/**
	 * A transient copy of Source mirrored on each axis Scale has negative, or null with OutWhy set (the mesh
	 * has no readable geometry, Scale has no negative axis). OutPositiveScale is the scale to place the copy
	 * at: abs(Scale), set on every return. Mirroring is about the mesh's own origin, as a negative scale is.
	 *
	 * Cached by (Source, which axes are negative): ten instances of a mesh mirrored on X share one copy, whatever
	 * their other scales. A source that changed since (its geometry or its material slots) is mirrored again.
	 */
	static UStaticMesh* MirroredCopy(UStaticMesh* Source, const FVector& Scale, FVector& OutPositiveScale, FString& OutWhy);

	/**
	 * To's simple collision as From's, mirrored on each negative axis of Scale (boxes, spheres, capsules, tapered
	 * capsules and convex hulls; level sets are left out), with From's collision complexity.
	 */
	static void MirrorCollision(const UBodySetup& From, UBodySetup& To, const FVector& Scale);

	/** Forgets every cached copy (tests; the editor's shutdown). */
	static void ClearCache();
};
