// Copyright Joshua Eger. All Rights Reserved.
#include "MantleWeaponFit.h"

namespace MantleWeaponFitImpl
{
	struct FHoldInfo
	{
		const TCHAR* Name;
		float Typical;
		float Min;
		float Max;
	};

	// Overall lengths in centimetres, muzzle to butt: real-world guns of each kind, with a margin either side so a
	// stylised mesh is not flagged for being a little off.
	static const FHoldInfo& HoldInfo(EMantleWeaponFitHold Hold)
	{
		static const FHoldInfo Table[] = {
			{TEXT("pistol"), 20.f, 14.f, 30.f},
			{TEXT("revolver"), 28.f, 18.f, 40.f},
			{TEXT("rifle"), 85.f, 60.f, 110.f},
			{TEXT("SMG"), 60.f, 35.f, 80.f},
			{TEXT("DMR"), 105.f, 80.f, 125.f},
			{TEXT("pump shotgun"), 100.f, 75.f, 125.f},
			{TEXT("bolt sniper"), 120.f, 85.f, 145.f}, // the pack's own SRM-12 is 91 cm
			{TEXT("LMG"), 110.f, 85.f, 135.f},
			{TEXT("one-handed blade"), 90.f, 20.f, 130.f},
			{TEXT("two-handed blade"), 140.f, 90.f, 200.f},
			{TEXT("spear"), 210.f, 130.f, 300.f},
			{TEXT("staff"), 180.f, 100.f, 250.f},
			{TEXT("bow"), 150.f, 70.f, 210.f},
			{TEXT("fists"), 20.f, 5.f, 40.f},
			{TEXT("shield"), 70.f, 25.f, 130.f},
			{TEXT("throwable"), 15.f, 3.f, 70.f},
			{TEXT("consumable"), 15.f, 4.f, 35.f},
		};
		static_assert(UE_ARRAY_COUNT(Table) == static_cast<int32>(EMantleWeaponFitHold::Count), "one size per hold");
		return Table[FMath::Clamp<int32>(static_cast<int32>(Hold), 0, UE_ARRAY_COUNT(Table) - 1)];
	}

	// Where Grip_R sits along the gun as a fraction of its length from the rear, for the Proportion rule. Measured on two sample
	// guns only (a 77.25 cm long gun and a 22.42 cm one-handed gun); the rest are held out as the check, so nothing else may
	// tune these.
	static constexpr double LongGunGripFraction = 0.2705;
	static constexpr double OneHandedGripFraction = 0.0709;

	/** Half-width of the slab (fraction of length along the bore) the Proportion rule reads the underside from. */
	static constexpr double GripSlabHalfWidth = 0.015;

	/** How far back up from the slab's lowest point toward the body underside the hand sits, so it is inside the grip. */
	static constexpr double GripRaiseFraction = 0.25;

	static bool IsTwoHanded(EMantleWeaponFitHold Hold)
	{
		return Hold != EMantleWeaponFitHold::Pistol && Hold != EMantleWeaponFitHold::Revolver;
	}

	/** Cyclic Jacobi eigen solve of a symmetric 3x3 matrix (destroyed). OutAxes are the unit eigenvectors, largest eigenvalue first. */
	static void SymmetricEigen(double A[3][3], FVector OutAxes[3])
	{
		double V[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
		static const int32 Pairs[3][2] = {{0, 1}, {0, 2}, {1, 2}};
		for (int32 Sweep = 0; Sweep < 50; ++Sweep)
		{
			const double Off = FMath::Abs(A[0][1]) + FMath::Abs(A[0][2]) + FMath::Abs(A[1][2]);
			const double Diag = FMath::Abs(A[0][0]) + FMath::Abs(A[1][1]) + FMath::Abs(A[2][2]);
			if (Off <= 1.0e-14 * Diag || Off <= 1.0e-300)
			{
				break;
			}
			for (const int32* Pair : Pairs)
			{
				const int32 P = Pair[0];
				const int32 Q = Pair[1];
				if (FMath::Abs(A[P][Q]) <= 1.0e-300)
				{
					continue;
				}
				const double Theta = (A[Q][Q] - A[P][P]) / (2.0 * A[P][Q]);
				const double T = (Theta >= 0.0 ? 1.0 : -1.0) / (FMath::Abs(Theta) + FMath::Sqrt(Theta * Theta + 1.0));
				const double C = 1.0 / FMath::Sqrt(T * T + 1.0);
				const double S = T * C;
				for (int32 K = 0; K < 3; ++K)
				{
					const double Kp = A[K][P];
					const double Kq = A[K][Q];
					A[K][P] = C * Kp - S * Kq;
					A[K][Q] = S * Kp + C * Kq;
				}
				for (int32 K = 0; K < 3; ++K)
				{
					const double Pk = A[P][K];
					const double Qk = A[Q][K];
					A[P][K] = C * Pk - S * Qk;
					A[Q][K] = S * Pk + C * Qk;
				}
				for (int32 K = 0; K < 3; ++K)
				{
					const double Vp = V[K][P];
					const double Vq = V[K][Q];
					V[K][P] = C * Vp - S * Vq;
					V[K][Q] = S * Vp + C * Vq;
				}
			}
		}
		int32 Order[3] = {0, 1, 2};
		for (int32 I = 0; I < 3; ++I)
		{
			for (int32 J = I + 1; J < 3; ++J)
			{
				if (A[Order[J]][Order[J]] > A[Order[I]][Order[I]])
				{
					Swap(Order[I], Order[J]);
				}
			}
		}
		for (int32 I = 0; I < 3; ++I)
		{
			OutAxes[I] = FVector(V[0][Order[I]], V[1][Order[I]], V[2][Order[I]]).GetSafeNormal();
		}
	}

	/** The range of the points along a direction. */
	static FVector2D ExtentAlong(const TArray<FVector>& Pts, const FVector& Dir, int32 Stride = 1)
	{
		double Lo = TNumericLimits<double>::Max();
		double Hi = TNumericLimits<double>::Lowest();
		for (int32 I = 0; I < Pts.Num(); I += Stride)
		{
			const double D = Pts[I] | Dir;
			Lo = FMath::Min(Lo, D);
			Hi = FMath::Max(Hi, D);
		}
		return FVector2D(Lo, Hi);
	}

	/** The box around the points whose X is in [X0, X1]. False when there are none. */
	static bool SlabBox(const TArray<FVector>& Pts, double X0, double X1, FBox& OutBox)
	{
		OutBox.Init();
		for (const FVector& P : Pts)
		{
			if (P.X >= X0 && P.X <= X1)
			{
				OutBox += P;
			}
		}
		return OutBox.IsValid != 0;
	}

	/** Cross-section size of a slab: lateral extent times height. Empty slabs count as huge so they never win as the muzzle. */
	static double SlabArea(const TArray<FVector>& Pts, double X0, double X1)
	{
		FBox Box;
		if (!SlabBox(Pts, X0, X1, Box))
		{
			return TNumericLimits<double>::Max();
		}
		return (Box.Max.Y - Box.Min.Y) * (Box.Max.Z - Box.Min.Z);
	}

	/** A run of adjacent length bins that hang below the body: a pistol grip, a magazine, a stock. */
	struct FRun
	{
		int32 Begin = 0;
		int32 End = 0;	  // inclusive
		double LowestZ = 0.0;
		double MinY = 0.0;
		double MaxY = 0.0;
	};

	static FString LengthWords(double Cm)
	{
		return Cm >= 100.0 ? FString::Printf(TEXT("%.1f m"), Cm / 100.0) : FString::Printf(TEXT("%.0f cm"), Cm);
	}

	static bool FitsHold(EMantleWeaponFitHold Hold, double Cm)
	{
		return Cm >= HoldInfo(Hold).Min && Cm <= HoldInfo(Hold).Max;
	}

	/** The hold other than Skip whose range holds Cm and whose typical length is nearest it, or Count when none does. */
	static EMantleWeaponFitHold NearestFittingHold(double Cm, EMantleWeaponFitHold Skip)
	{
		EMantleWeaponFitHold Best = EMantleWeaponFitHold::Count;
		double BestDistance = TNumericLimits<double>::Max();
		for (int32 H = 0; H < static_cast<int32>(EMantleWeaponFitHold::Count); ++H)
		{
			const EMantleWeaponFitHold Other = static_cast<EMantleWeaponFitHold>(H);
			if (Other == Skip || !FitsHold(Other, Cm))
			{
				continue;
			}
			const double Distance = FMath::Abs(FMath::Loge(Cm / HoldInfo(Other).Typical));
			if (Distance < BestDistance)
			{
				BestDistance = Distance;
				Best = Other;
			}
		}
		return Best;
	}

	struct FScaleDecision
	{
		double Scale = 1.0;
		FString Reason;
		FString Suggestion;
		bool bOutsideRange = false;
	};

	/** The "unusual for this hold" message: the real length, short or long, and the hold it fits (or the units, when none does). */
	static FString UnusualLengthMessage(EMantleWeaponFitHold Current, double Cm)
	{
		const FHoldInfo& Info = HoldInfo(Current);
		const FString Head = FString::Printf(TEXT("%s is %s for a %s (%.0f to %.0f cm)."), *LengthWords(Cm), Cm < Info.Min ? TEXT("short") : TEXT("long"), Info.Name, Info.Min, Info.Max);
		const EMantleWeaponFitHold Named = NearestFittingHold(Cm, Current);
		if (Named == EMantleWeaponFitHold::Count)
		{
			return Head + TEXT(" It is not the right size for any hold, so it looks like the wrong units. Fix the scale when you import the model.");
		}
		return FString::Printf(TEXT("%s It fits the %s hold%s."), *Head, HoldInfo(Named).Name,
			Current == EMantleWeaponFitHold::Throwable && Named == EMantleWeaponFitHold::Spear ? TEXT(", which can throw the weapon model itself") : TEXT(""));
	}

	/**
	 * A unit slip is a model whose length is not a plausible size for ANY hold. Only then is it rescaled: by the one conversion that
	 * lands in the chosen hold's range, else in any hold's range (the one nearest a typical length). A length that fits some hold
	 * keeps its size; when that hold is not the chosen one, the message says so.
	 */
	static FScaleDecision DecideScale(EMantleWeaponFitHold Current, double RawCm)
	{
		FScaleDecision Out;
		const FHoldInfo& Info = HoldInfo(Current);
		if (FitsHold(Current, RawCm))
		{
			Out.Reason = FString::Printf(TEXT("Kept at 1.0x: %s is a normal %s length"), *LengthWords(RawCm), Info.Name);
			return Out;
		}
		if (NearestFittingHold(RawCm, Current) != EMantleWeaponFitHold::Count)
		{
			Out.bOutsideRange = true;
			Out.Reason = FString::Printf(TEXT("Kept at 1.0x: %s is %s for a %s"), *LengthWords(RawCm), RawCm < Info.Min ? TEXT("short") : TEXT("long"), Info.Name);
			Out.Suggestion = UnusualLengthMessage(Current, RawCm);
			return Out;
		}
		struct FUnitFix
		{
			double Factor;
			const TCHAR* Made;
		};
		static const FUnitFix Fixes[] = {
			{100.0, TEXT("in metres")},
			{2.54, TEXT("in inches")},
			{0.1, TEXT("in millimetres")},
			{0.01, TEXT("100 times too large")},
		};
		const FUnitFix* Chosen = nullptr;
		for (const bool bChosenHoldOnly : { true, false })
		{
			double ChosenDistance = TNumericLimits<double>::Max();
			for (const FUnitFix& Fix : Fixes)
			{
				const double Fixed = RawCm * Fix.Factor;
				for (int32 H = 0; H < static_cast<int32>(EMantleWeaponFitHold::Count); ++H)
				{
					const EMantleWeaponFitHold Other = static_cast<EMantleWeaponFitHold>(H);
					if ((bChosenHoldOnly && Other != Current) || !FitsHold(Other, Fixed))
					{
						continue;
					}
					const double Distance = FMath::Abs(FMath::Loge(Fixed / HoldInfo(Other).Typical));
					if (Distance < ChosenDistance)
					{
						ChosenDistance = Distance;
						Chosen = &Fix;
					}
				}
			}
			if (Chosen)
			{
				break;
			}
		}
		if (!Chosen)
		{
			Out.bOutsideRange = true;
			Out.Reason = FString::Printf(TEXT("Kept at 1.0x: %s is %s for a %s"), *LengthWords(RawCm), RawCm < Info.Min ? TEXT("short") : TEXT("long"), Info.Name);
			Out.Suggestion = UnusualLengthMessage(Current, RawCm);
			return Out;
		}
		Out.Scale = Chosen->Factor;
		const double Fixed = RawCm * Chosen->Factor;
		Out.Reason = FString::Printf(TEXT("Scaled %gx: the mesh looks like it was made %s (%.3g of its units long, now %.0f cm)"), Chosen->Factor, Chosen->Made, RawCm, Fixed);
		if (!FitsHold(Current, Fixed))
		{
			Out.bOutsideRange = true;
			Out.Suggestion = UnusualLengthMessage(Current, Fixed);
		}
		return Out;
	}
}

FMantleWeaponFitResult FMantleWeaponFit::Fit(const FMantleWeaponFitInput& In)
{
	using namespace MantleWeaponFitImpl;

	FMantleWeaponFitResult Result;
	if (In.Positions.Num() < 3)
	{
		Result.ScaleReason = TEXT("Kept at 1.0x: the mesh has too few vertices to measure.");
		Result.Notes.Add(TEXT("The mesh has almost no geometry, so nothing could be fitted. Its points are at the origin; move them by hand."));
		return Result;
	}

	// 1. The point cloud: the vertices, plus samples along each triangle edge when triangles are given, so sparse low-poly
	//    meshes (a barrel that is one long box) measure like dense ones. Spacing is 1% of the mesh's overall size.
	TArray<FVector> Pts;
	Pts.Reserve(In.Positions.Num());
	FBox Bounds(ForceInit);
	for (const FVector3f& P : In.Positions)
	{
		const FVector V(P);
		Pts.Add(V);
		Bounds += V;
	}
	const double Diagonal = Bounds.GetSize().Size();
	if (Diagonal <= UE_DOUBLE_SMALL_NUMBER)
	{
		Result.ScaleReason = TEXT("Kept at 1.0x: every vertex of the mesh is in the same place.");
		Result.Notes.Add(TEXT("The mesh has no size, so nothing could be fitted. Its points are at the origin; move them by hand."));
		return Result;
	}
	const double Spacing = Diagonal / 100.0;
	for (int32 T = 0; T + 2 < In.Triangles.Num() && Pts.Num() < 2000000; T += 3)
	{
		for (int32 E = 0; E < 3; ++E)
		{
			const int32 IndexA = In.Triangles[T + E];
			const int32 IndexB = In.Triangles[T + (E + 1) % 3];
			if (!In.Positions.IsValidIndex(IndexA) || !In.Positions.IsValidIndex(IndexB))
			{
				continue;
			}
			const FVector A(In.Positions[IndexA]);
			const FVector B(In.Positions[IndexB]);
			const int32 Steps = FMath::Min(FMath::CeilToInt32((B - A).Size() / Spacing), 64);
			for (int32 S = 1; S < Steps; ++S)
			{
				const FVector Sample = FMath::Lerp(A, B, static_cast<double>(S) / Steps);
				Pts.Add(Sample);
			}
		}
	}

	// 2. The long axis: the principal axis of the cloud (largest eigenvector of its covariance).
	FVector Centroid = FVector::ZeroVector;
	for (const FVector& P : Pts)
	{
		Centroid += P;
	}
	Centroid /= static_cast<double>(Pts.Num());
	double Cov[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
	for (const FVector& P : Pts)
	{
		const FVector D = P - Centroid;
		const double Comp[3] = {D.X, D.Y, D.Z};
		for (int32 R = 0; R < 3; ++R)
		{
			for (int32 C = 0; C < 3; ++C)
			{
				Cov[R][C] += Comp[R] * Comp[C];
			}
		}
	}
	FVector Axes[3];
	SymmetricEigen(Cov, Axes);

	// A throwable is held by a handle only when it has one: a knife or a hatchet is one axis much longer than the other two, and the hand holds the
	// handle end. A compact one (a grenade, a ball, a star) is held at its middle.
	// The handle is found like a blade's (the widest end), so a hatchet's head is taken for its handle: the creator's Flip covers that.
	bool bHandled = false;
	if (In.Hold == EMantleWeaponFitHold::Throwable)
	{
		const FVector2D ExtentLong = ExtentAlong(Pts, Axes[0]);
		const FVector2D ExtentMid = ExtentAlong(Pts, Axes[1]);
		const FVector2D ExtentShort = ExtentAlong(Pts, Axes[2]);
		const double Other = FMath::Max(ExtentMid.Y - ExtentMid.X, ExtentShort.Y - ExtentShort.X);
		bHandled = (ExtentLong.Y - ExtentLong.X) >= FMantleWeaponFit::ElongatedThrowableRatio * FMath::Max(Other, UE_DOUBLE_SMALL_NUMBER);
	}

	// The non-firearm families use their shaft/blade axis, or the bow's limbs/shield's face, rather than gun underside rules.
	if (In.Hold >= EMantleWeaponFitHold::Melee1h)
	{
		const bool bBow = In.Hold == EMantleWeaponFitHold::Bow;
		const bool bShield = In.Hold == EMantleWeaponFitHold::Shield;
		const bool bStaff = In.Hold == EMantleWeaponFitHold::Staff;
		FVector X = bBow ? Axes[1] : bShield ? Axes[2] : Axes[0];
		FVector Z = (bBow || bShield) ? Axes[0] : Axes[1];
		// Stable sign on symmetric models; the creator confirms the front and handle with Flip/Turn and the points.
		auto StableSign = [](FVector V)
		{
			const FVector A = V.GetAbs();
			const double Main = A.X >= A.Y && A.X >= A.Z ? V.X : A.Y >= A.Z ? V.Y : V.Z;
			return Main < 0.0 ? -V : V;
		};
		X = StableSign(X);
		Z = StableSign(Z);
		// A blade runs handle to tip along +X. The handle is the end with the crossguard: the widest cross-section in the
		// outer third at either end (a tip tapers to nothing), whatever way the model happened to face in its file. The
		// creator's Flip (bReverseLength) puts the handle at the other end.
		if (In.Hold == EMantleWeaponFitHold::Melee1h || In.Hold == EMantleWeaponFitHold::Melee2h || bHandled)
		{
			double Lo = TNumericLimits<double>::Max(), Hi = TNumericLimits<double>::Lowest();
			for (const FVector& P : Pts) { const double A = (P - Centroid) | X; Lo = FMath::Min(Lo, A); Hi = FMath::Max(Hi, A); }
			const double Third = (Hi - Lo) / 3.0;
			double WidthLo = 0.0, WidthHi = 0.0;
			for (const FVector& P : Pts)
			{
				const FVector D = P - Centroid;
				const double A = D | X;
				const double Across = (D - X * A).Size();
				if (A <= Lo + Third) WidthLo = FMath::Max(WidthLo, Across);
				if (A >= Hi - Third) WidthHi = FMath::Max(WidthHi, Across);
			}
			if ((WidthHi > WidthLo) != In.bReverseLength)
			{
				X = -X;
			}
		}
		// A staff's front is its head: the end of its length with the bulkier cross-section (an orb, a crystal or a curl against a plain butt),
		// compared over the outer share of the length at each end. The creator's Flip (bReverseLength) puts the head at the other end.
		if (bStaff)
		{
			double Lo = TNumericLimits<double>::Max(), Hi = TNumericLimits<double>::Lowest();
			for (const FVector& P : Pts) { const double A = (P - Centroid) | X; Lo = FMath::Min(Lo, A); Hi = FMath::Max(Hi, A); }
			const double EndSpan = (Hi - Lo) * FMantleWeaponFit::StaffEndShare;
			const FVector Across1 = FMath::Abs(X | Axes[1]) < 0.9 ? Axes[1] : Axes[2];
			const FVector Across2 = (X ^ Across1).GetSafeNormal();
			FBox2D CrossLo(ForceInit), CrossHi(ForceInit);
			for (const FVector& P : Pts)
			{
				const FVector D = P - Centroid;
				const double A = D | X;
				if (A <= Lo + EndSpan) CrossLo += FVector2D(D | Across1, D | Across2);
				if (A >= Hi - EndSpan) CrossHi += FVector2D(D | Across1, D | Across2);
			}
			const double AreaLo = CrossLo.bIsValid ? CrossLo.GetArea() : 0.0;
			const double AreaHi = CrossHi.bIsValid ? CrossHi.GetArea() : 0.0;
			if ((AreaLo > AreaHi) != In.bReverseLength)
			{
				X = -X;
			}
		}
		// A bow's front is the riser's side: the string is on the archer's side of the limbs, so the middle of the length sits further
		// forward than the limbs between the riser and the tips (the tips themselves may curl either way: a recurve's curl forward).
		// The creator's Flip (bReverseLength) turns it.
		if (bBow)
		{
			double Lo = TNumericLimits<double>::Max(), Hi = TNumericLimits<double>::Lowest();
			for (const FVector& P : Pts) { const double A = (P - Centroid) | Z; Lo = FMath::Min(Lo, A); Hi = FMath::Max(Hi, A); }
			const double Span = FMath::Max(Hi - Lo, UE_DOUBLE_SMALL_NUMBER);
			double RiserSum = 0.0, LimbSum = 0.0;
			int32 Riser = 0, Limb = 0;
			for (const FVector& P : Pts)
			{
				const FVector D = P - Centroid;
				const double Along = FMath::Abs((D | Z) - 0.5 * (Lo + Hi)) / Span;
				if (Along <= FMantleWeaponFit::BowRiserHalfShare) { RiserSum += D | X; ++Riser; }
				else if (Along >= 0.15 && Along <= 0.4) { LimbSum += D | X; ++Limb; }
			}
			const bool bFacesBack = Riser > 0 && Limb > 0 && RiserSum / Riser < LimbSum / Limb;
			if (bFacesBack != In.bReverseLength)
			{
				X = -X;
			}
		}
		// A shield faces +X: its face is the convex side, where the middle stands out past the rim (most shields bulge);
		// a flat one keeps the stable sign, and the creator's Flip turns it.
		if (bShield)
		{
			double Radius = 0.0;
			for (const FVector& P : Pts) { const FVector D = P - Centroid; Radius = FMath::Max(Radius, (D - X * (D | X)).Size()); }
			double MiddleSum = 0.0, RimSum = 0.0;
			int32 Middle = 0, Rim = 0;
			for (const FVector& P : Pts)
			{
				const FVector D = P - Centroid;
				const double A = D | X;
				const double Across = (D - X * A).Size();
				if (Across < 0.25 * Radius) { MiddleSum += A; ++Middle; }
				else if (Across > 0.75 * Radius) { RimSum += A; ++Rim; }
			}
			if (Middle > 0 && Rim > 0 && MiddleSum / Middle < RimSum / Rim)
			{
				X = -X;
			}
		}
		const FVector Y = (Z ^ X).GetSafeNormal();
		Z = (X ^ Y).GetSafeNormal();
		const FMatrix Frame(FVector(X.X, Y.X, Z.X), FVector(X.Y, Y.Y, Z.Y), FVector(X.Z, Y.Z, Z.Z), FVector::ZeroVector);
		FBox Box(ForceInit);
		for (const auto& P : Pts) Box += FVector((P - Centroid) | X, (P - Centroid) | Y, (P - Centroid) | Z);
		const double Length = (bBow || bShield) ? Box.GetSize().Z : Box.GetSize().X;
		const FScaleDecision Decision = DecideScale(In.Hold, Length);
		const double Scale = Decision.Scale;
		Result.Scale = static_cast<float>(Scale);
		Result.LengthCm = static_cast<float>(Length * Scale);
		Result.bOutsideTypicalRange = Decision.bOutsideRange;
		Result.Suggestion = Decision.Suggestion;
		Result.ScaleReason = Decision.Reason;
		// A throwable longer than a throwing weapon's range (a javelin, a spear) is fitted like the Spear hold: the shaft grip and the second hand.
		const bool bLongThrowable = bHandled && In.Hold == EMantleWeaponFitHold::Throwable && Length > HoldInfo(In.Hold).Max;
		const FVector Centre = Box.GetCenter();
		FVector Grip = Centre;
		// A bow's riser (the middle of its length): the palm goes behind its front face and past its hand-side face (-Y), the arrow
		// lies along its other face.
		double BowArrowY = Centre.Y;
		if (bBow)
		{
			double FrontX = TNumericLimits<double>::Lowest(), HandY = TNumericLimits<double>::Max(), ArrowY = TNumericLimits<double>::Lowest();
			for (const FVector& P : Pts)
			{
				const FVector L((P - Centroid) | X, (P - Centroid) | Y, (P - Centroid) | Z);
				if (FMath::Abs(L.Z - Centre.Z) <= FMantleWeaponFit::BowRiserHalfShare * Length)
				{
					FrontX = FMath::Max(FrontX, L.X);
					HandY = FMath::Min(HandY, L.Y);
					ArrowY = FMath::Max(ArrowY, L.Y);
				}
			}
			if (FrontX == TNumericLimits<double>::Lowest()) { FrontX = Box.Max.X; HandY = Box.Min.Y; ArrowY = Box.Max.Y; }
			Grip = FVector(FrontX - FMantleWeaponFit::BowPalmBehindFrontCm / Scale, HandY - FMantleWeaponFit::BowPalmBeyondRiserCm / Scale,
				Centre.Z - FMantleWeaponFit::BowGripBelowMiddleShare * Length);
			BowArrowY = ArrowY - FMantleWeaponFit::BowNockInsideRiserCm / Scale;
		}
		if (!bBow && !bShield && (In.Hold != EMantleWeaponFitHold::Throwable || bHandled) && In.Hold != EMantleWeaponFitHold::Unarmed)
		{
			// A handled throwable's hand is on the handle the way a one-handed blade's is (the handle is the first 12% of it); a compact one keeps the middle.
			const bool bShaft = In.Hold == EMantleWeaponFitHold::Spear || bLongThrowable;
			Grip.X = Box.Min.X + Box.GetSize().X * (bStaff ? FMantleWeaponFit::StaffGripFromButtShare : bShaft ? 0.35 : 0.12);
		}
		// A staff's palm is on the shaft's middle line, not the middle of the box (a curled head pulls the box's middle off the shaft): the
		// middle of the mesh just around the palm. Its Focus is the middle of the head's top, the outer share of the length.
		double StaffFocusY = Centre.Y, StaffFocusZ = Centre.Z;
		if (bStaff)
		{
			FBox2D Shaft(ForceInit), Head(ForceInit);
			for (const FVector& P : Pts)
			{
				const FVector L((P - Centroid) | X, (P - Centroid) | Y, (P - Centroid) | Z);
				if (FMath::Abs(L.X - Grip.X) <= FMantleWeaponFit::StaffShaftSampleShare * Length) Shaft += FVector2D(L.Y, L.Z);
				if (L.X >= Box.Max.X - FMantleWeaponFit::StaffFocusRegionShare * Length) Head += FVector2D(L.Y, L.Z);
			}
			if (Shaft.bIsValid) { Grip.Y = Shaft.GetCenter().X; Grip.Z = Shaft.GetCenter().Y; }
			if (Head.bIsValid) { StaffFocusY = Head.GetCenter().X; StaffFocusZ = Head.GetCenter().Y; }
		}
		// A shield's Grip is the middle of the left palm, just behind the back of the shield at its middle
		// (FMantleWeaponFit::ShieldPalmBehindCm): the back measured there, not the box's, which horns or spikes curling
		// behind the rim push out.
		if (bShield)
		{
			const double Reach = 0.15 * FMath::Max(Box.GetSize().Y, Box.GetSize().Z);
			double Back = TNumericLimits<double>::Max();
			for (const FVector& P : Pts)
			{
				const FVector L((P - Centroid) | X, (P - Centroid) | Y, (P - Centroid) | Z);
				if (FVector2D(L.Y - Centre.Y, L.Z - Centre.Z).Size() < Reach) Back = FMath::Min(Back, L.X);
			}
			Grip.X = (Back < TNumericLimits<double>::Max() ? Back : Box.Min.X) - ShieldPalmBehindCm / Scale;
		}
		const FVector Origin = Grip * Scale;
		Result.GripFromRearCm = static_cast<float>((Grip.X - Box.Min.X) * Scale);
		Result.MeshToWeapon = FTransform(FQuat(Frame), -FVector(Centroid | X, Centroid | Y, Centroid | Z) * Scale - Origin, FVector(Scale));
		auto Point = [&](const TCHAR* Role, FVector P) { Result.Points.Add(FName(Role), FTransform(FQuat::Identity, P * Scale - Origin)); };
		if (In.Hold == EMantleWeaponFitHold::Consumable)
		{
			// An item held whole in the palm: its middle is the grip.
			Point(TEXT("Grip"), Centre);
		}
		else if (bShield)
		{
			Point(TEXT("Grip"), Grip);
			Point(TEXT("Face"), FVector(Box.Max.X, Centre.Y, Centre.Z));
		}
		else if (bBow)
		{
			// The string runs in the plane just inside the limb tips and the nock rests on it a little above the middle.
			const double Depth = Box.GetSize().X;
			Point(TEXT("Grip_L"), Grip);
			Point(TEXT("StringTop"), FVector(Box.Min.X + FMantleWeaponFit::BowStringPlaneShare * Depth, BowArrowY, Box.Max.Z));
			Point(TEXT("StringBottom"), FVector(Box.Min.X + FMantleWeaponFit::BowStringPlaneShare * Depth, BowArrowY, Box.Min.Z));
			Point(TEXT("Nock"), FVector(Box.Min.X + FMantleWeaponFit::BowNockPlaneShare * Depth, BowArrowY, Centre.Z + FMantleWeaponFit::BowNockAboveMiddleShare * Length));
		}
		else
		{
			// A blade's right hand point is the middle of the palm, which rests on the handle's -Z side: the handle sits in the
			// fist that far from it (measured on the melee holds' clips).
			const bool bBlade = In.Hold == EMantleWeaponFitHold::Melee1h || In.Hold == EMantleWeaponFitHold::Melee2h;
			Point(TEXT("Grip_R"), bBlade ? Grip - FVector(0.0, 0.0, BladePalmFromHandleCm / Scale) : Grip);
			if (bStaff)
				Point(TEXT("Grip_L"), Grip - FVector(Box.GetSize().X * FMantleWeaponFit::StaffLeftHandBehindShare, 0.0, 0.0));
			else if (In.Hold == EMantleWeaponFitHold::Melee2h || In.Hold == EMantleWeaponFitHold::Spear || bLongThrowable)
				Point(TEXT("Grip_L"), Grip + FVector(Box.GetSize().X * (In.Hold == EMantleWeaponFitHold::Melee2h ? 0.08 : 0.15), 0.0, 0.0));
			if (bStaff) Point(TEXT("Focus"), FVector(Box.Max.X, StaffFocusY, StaffFocusZ));
			else if (In.Hold != EMantleWeaponFitHold::Unarmed)
			{
				Point(TEXT("Tip"), FVector(Box.Max.X, Centre.Y, Centre.Z));
				if (In.Hold != EMantleWeaponFitHold::Throwable)
				{
					Point(TEXT("EdgeBase"), FVector(Box.Min.X + Box.GetSize().X * (In.Hold == EMantleWeaponFitHold::Spear ? 0.85 : 0.25), Centre.Y, Centre.Z));
					Point(TEXT("EdgeTip"), FVector(Box.Max.X, Centre.Y, Centre.Z));
				}
			}
		}
		Result.Notes.Add(TEXT("Check the guessed handle, front and contact points on your model before creating the weapon."));
		return Result;
	}

	// Of the other two axes the taller one is up: guns are taller than they are wide.
	const FVector2D ExtentB = ExtentAlong(Pts, Axes[1]);
	const FVector2D ExtentC = ExtentAlong(Pts, Axes[2]);
	const bool bFirstIsUp = (ExtentB.Y - ExtentB.X) >= (ExtentC.Y - ExtentC.X);
	const FVector UpRaw = bFirstIsUp ? Axes[1] : Axes[2];

	// A grip below the rear pulls the principal axis off the bore. Sweep a small rotation in the long/up plane and keep
	// the one with the tightest box around the gun; ties go to the smallest rotation. Uses at most ~20000 points.
	const int32 Stride = FMath::Max(1, Pts.Num() / 20000);
	FVector Long = Axes[0];
	FVector Up = UpRaw;
	{
		double BestArea = TNumericLimits<double>::Max();
		int32 BestStep = 0;
		for (int32 Step = 0; Step <= 100; ++Step)
		{
			// Visit 0, +1, -1, +2, -2, ... so the first of equal areas is the smallest rotation.
			const int32 Index = (Step % 2 == 1) ? (Step + 1) / 2 : -(Step / 2);
			const double Angle = FMath::DegreesToRadians(Index * 0.5);
			const FVector TryLong = Axes[0] * FMath::Cos(Angle) + UpRaw * FMath::Sin(Angle);
			const FVector TryUp = UpRaw * FMath::Cos(Angle) - Axes[0] * FMath::Sin(Angle);
			const FVector2D L = ExtentAlong(Pts, TryLong, Stride);
			const FVector2D U = ExtentAlong(Pts, TryUp, Stride);
			const double Area = (L.Y - L.X) * (U.Y - U.X);
			if (Area < BestArea * (1.0 - 1.0e-9))
			{
				BestArea = Area;
				BestStep = Index;
			}
		}
		const double Angle = FMath::DegreesToRadians(BestStep * 0.5);
		Long = (Axes[0] * FMath::Cos(Angle) + UpRaw * FMath::Sin(Angle)).GetSafeNormal();
		Up = (UpRaw * FMath::Cos(Angle) - Axes[0] * FMath::Sin(Angle)).GetSafeNormal();
	}
	FVector Side = (Long ^ Up).GetSafeNormal();

	// The cloud in (long, side, up) coordinates around the centroid, still in mesh units. Decisions below read this.
	TArray<FVector> Measured;
	Measured.Reserve(Pts.Num());
	for (const FVector& P : Pts)
	{
		const FVector D = P - Centroid;
		Measured.Add(FVector(D | Long, D | Side, D | Up));
	}
	double MinLong = TNumericLimits<double>::Max();
	double MaxLong = TNumericLimits<double>::Lowest();
	for (const FVector& P : Measured)
	{
		MinLong = FMath::Min(MinLong, P.X);
		MaxLong = FMath::Max(MaxLong, P.X);
	}
	const double RawLength = MaxLong - MinLong;
	if (RawLength <= UE_DOUBLE_SMALL_NUMBER)
	{
		Result.ScaleReason = TEXT("Kept at 1.0x: the mesh has no length to measure.");
		Result.Notes.Add(TEXT("The mesh has no clear long axis, so nothing could be fitted. Its points are at the origin; move them by hand."));
		return Result;
	}

	// 3. Up sign: the grip and magazine hang off the bore. The bore line is the height of the muzzle-end slab (front 5% of
	//    the length), since the butt end of a real gun sits low and would drag the line down. The side whose farthest vertex
	//    is farther from that line is down (grips and magazines hang much further than sights and rails rise). Within 15%
	//    the moment of the vertices about the line decides, with a note to check.
	{
		// Which end is the muzzle is settled properly in step 4; here the smaller end (by area) is enough, and a near-tie goes to
		// the end away from the vertex farthest off the middle height, which is the grip or magazine whichever way up the mesh is.
		const double AreaRearProv = SlabArea(Measured, MinLong, MinLong + 0.1 * RawLength);
		const double AreaFrontProv = SlabArea(Measured, MaxLong - 0.1 * RawLength, MaxLong);
		bool bProvisionalMuzzleAtMax = AreaFrontProv < AreaRearProv;
		const double LargerProv = FMath::Max(AreaRearProv, AreaFrontProv);
		if (LargerProv < TNumericLimits<double>::Max() && FMath::Abs(AreaRearProv - AreaFrontProv) < 0.15 * LargerProv)
		{
			double FarthestX = 0.0;
			double FarthestZ = -1.0;
			for (const FVector& P : Measured)
			{
				if (FMath::Abs(P.Z) > FarthestZ)
				{
					FarthestZ = FMath::Abs(P.Z);
					FarthestX = P.X;
				}
			}
			bProvisionalMuzzleAtMax = FMath::Abs(MaxLong - FarthestX) > FMath::Abs(MinLong - FarthestX);
		}

		double BoreLo = TNumericLimits<double>::Max();
		double BoreHi = TNumericLimits<double>::Lowest();
		for (const FVector& P : Measured)
		{
			const bool bInMuzzleSlab = bProvisionalMuzzleAtMax ? (P.X >= MaxLong - 0.05 * RawLength) : (P.X <= MinLong + 0.05 * RawLength);
			if (bInMuzzleSlab)
			{
				BoreLo = FMath::Min(BoreLo, P.Z);
				BoreHi = FMath::Max(BoreHi, P.Z);
			}
		}
		const double BoreZ = BoreLo <= BoreHi ? 0.5 * (BoreLo + BoreHi) : 0.0;
		double Above = 0.0;
		double Below = 0.0;
		double Moment = 0.0;
		for (const FVector& P : Measured)
		{
			const double Offset = P.Z - BoreZ;
			Above = FMath::Max(Above, Offset);
			Below = FMath::Max(Below, -Offset);
			Moment += Offset;
		}
		const double Farther = FMath::Max(Above, Below);
		bool bFlip = false;
		if (Farther > 0.0 && FMath::Abs(Above - Below) >= 0.15 * Farther)
		{
			// What reaches farther is down; if that is the side called up, flip.
			bFlip = Above > Below;
		}
		else
		{
			bFlip = Moment > 0.0;
			Result.Notes.Add(TEXT("It is hard to tell which way is up on this mesh (the grip and sights hang about as far from the barrel). Check the direction: the grip should point down."));
		}
		if (bFlip)
		{
			Up = -Up;
			Side = -Side;
			for (FVector& P : Measured)
			{
				P.Y = -P.Y;
				P.Z = -P.Z;
			}
		}
	}

	// 4. Muzzle end: the end whose last 10% has the smaller cross-section. Near-ties (within 15%) go to the end farther
	//    from the lowest point, which is where the grip is.
	bool bMuzzleAtMax = true;
	{
		const double AreaRear = SlabArea(Measured, MinLong, MinLong + 0.1 * RawLength);
		const double AreaFront = SlabArea(Measured, MaxLong - 0.1 * RawLength, MaxLong);
		bMuzzleAtMax = AreaFront < AreaRear;
		const double Larger = FMath::Max(AreaRear, AreaFront);
		if (Larger < TNumericLimits<double>::Max() && FMath::Abs(AreaRear - AreaFront) < 0.15 * Larger)
		{
			double LowestX = 0.0;
			double LowestZ = TNumericLimits<double>::Max();
			for (const FVector& P : Measured)
			{
				if (P.Z < LowestZ)
				{
					LowestZ = P.Z;
					LowestX = P.X;
				}
			}
			bMuzzleAtMax = FMath::Abs(MaxLong - LowestX) > FMath::Abs(MinLong - LowestX);
			Result.Notes.Add(TEXT("Both ends of the mesh look alike, so which end is the muzzle is a guess (the end away from the grip). Check the muzzle point and flip the mesh if it is wrong."));
		}
	}

	// The weapon frame's axes in mesh space: X toward the muzzle, Z up, Y = Z cross X, so it is always a proper rotation.
	const FVector AxisX = bMuzzleAtMax ? Long : -Long;
	const FVector AxisZ = Up;
	const FVector AxisY = AxisZ ^ AxisX;

	// 5. Scale. A unit slip is a length no hold takes (see DecideScale); a length that fits another hold keeps its size and is
	//    flagged with a message naming that hold.
	const FScaleDecision Decision = DecideScale(In.Hold, RawLength);
	const double Scale = Decision.Scale;
	Result.ScaleReason = Decision.Reason;
	Result.bOutsideTypicalRange = Decision.bOutsideRange;
	Result.Suggestion = Decision.Suggestion;
	Result.Scale = static_cast<float>(Scale);

	// The final cloud: weapon-frame axes, scaled to centimetres.
	TArray<FVector> Q;
	Q.Reserve(Pts.Num());
	double XMin = TNumericLimits<double>::Max();
	double XMax = TNumericLimits<double>::Lowest();
	for (const FVector& P : Pts)
	{
		const FVector D = P - Centroid;
		const FVector F(D | AxisX, D | AxisY, D | AxisZ);
		Q.Add(F * Scale);
		XMin = FMath::Min(XMin, F.X * Scale);
		XMax = FMath::Max(XMax, F.X * Scale);
	}
	const double Len = XMax - XMin;

	// 6. Points. The length is cut into bins; each keeps its lowest and highest point.
	const int32 NumBins = 60;
	const double BinWidth = Len / NumBins;
	struct FBin
	{
		double MinZ = TNumericLimits<double>::Max();
		double MaxZ = TNumericLimits<double>::Lowest();
		double MinY = TNumericLimits<double>::Max();
		double MaxY = TNumericLimits<double>::Lowest();
		bool bUsed = false;
	};
	TArray<FBin> Bins;
	Bins.SetNum(NumBins);
	for (const FVector& P : Q)
	{
		FBin& Bin = Bins[FMath::Clamp(FMath::FloorToInt32((P.X - XMin) / BinWidth), 0, NumBins - 1)];
		Bin.MinZ = FMath::Min(Bin.MinZ, P.Z);
		Bin.MaxZ = FMath::Max(Bin.MaxZ, P.Z);
		Bin.MinY = FMath::Min(Bin.MinY, P.Y);
		Bin.MaxY = FMath::Max(Bin.MaxY, P.Y);
		Bin.bUsed = true;
	}

	// The body's underside is the median of the bins' lowest points; anything hanging 5% of the length below it is a
	// protrusion (grip, magazine, stock). Adjacent such bins are one run.
	TArray<double> LowPoints;
	for (const FBin& Bin : Bins)
	{
		if (Bin.bUsed)
		{
			LowPoints.Add(Bin.MinZ);
		}
	}
	LowPoints.Sort();
	const double BodyBottom = LowPoints[LowPoints.Num() / 2];
	const double Hang = 0.05 * Len;
	TArray<FRun> Runs;
	for (int32 B = 0; B < NumBins; ++B)
	{
		const bool bHangs = Bins[B].bUsed && Bins[B].MinZ < BodyBottom - Hang;
		if (!bHangs)
		{
			continue;
		}
		if (Runs.Num() > 0 && Runs.Last().End == B - 1)
		{
			FRun& Run = Runs.Last();
			Run.End = B;
			Run.LowestZ = FMath::Min(Run.LowestZ, Bins[B].MinZ);
			Run.MinY = FMath::Min(Run.MinY, Bins[B].MinY);
			Run.MaxY = FMath::Max(Run.MaxY, Bins[B].MaxY);
		}
		else
		{
			FRun Run;
			Run.Begin = B;
			Run.End = B;
			Run.LowestZ = Bins[B].MinZ;
			Run.MinY = Bins[B].MinY;
			Run.MaxY = Bins[B].MaxY;
			Runs.Add(Run);
		}
	}
	auto RunWidth = [&](const FRun& Run) { return (Run.End - Run.Begin + 1) * BinWidth; };
	auto RunCentreX = [&](const FRun& Run) { return XMin + BinWidth * 0.5 * (Run.Begin + Run.End + 1); };

	// The grip is the rearmost protrusion in the rear 60% that is narrow enough to be a grip (a run over 25% of the
	// length is a stock or a handguard). The next protrusion ahead of it, within 25% of the length, is the magazine.
	int32 GripRun = INDEX_NONE;
	for (int32 R = 0; R < Runs.Num() && GripRun == INDEX_NONE; ++R)
	{
		if (RunCentreX(Runs[R]) < XMin + 0.6 * Len && RunWidth(Runs[R]) <= 0.25 * Len)
		{
			GripRun = R;
		}
	}
	FVector GripR;
	if (In.GripRule == EMantleWeaponGripRule::Proportion)
	{
		const double GripX = XMin + (IsTwoHanded(In.Hold) ? LongGunGripFraction : OneHandedGripFraction) * Len;
		double LowestZ = TNumericLimits<double>::Max();
		FBox Slab(ForceInit);
		for (const FVector& P : Q)
		{
			if (FMath::Abs(P.X - GripX) <= GripSlabHalfWidth * Len)
			{
				LowestZ = FMath::Min(LowestZ, P.Z);
				Slab += P;
			}
		}
		if (Slab.IsValid)
		{
			GripR = FVector(GripX, Slab.GetCenter().Y, LowestZ + GripRaiseFraction * FMath::Max(0.0, BodyBottom - LowestZ));
		}
		else
		{
			GripR = FVector(GripX, 0.0, BodyBottom);
		}
		Result.Notes.Add(TEXT("The right hand is placed by proportion along the gun, then down to the underside of the mesh there. Move it to where the hand should sit."));
	}
	else if (GripRun != INDEX_NONE)
	{
		const FRun& Run = Runs[GripRun];
		// Where the hand closes: just in front of the middle, a quarter of the way down from the body.
		GripR = FVector(RunCentreX(Run) + 0.15 * RunWidth(Run), 0.5 * (Run.MinY + Run.MaxY), BodyBottom - 0.25 * (BodyBottom - Run.LowestZ));
	}
	else
	{
		GripR = FVector(XMin + 0.25 * Len, 0.0, BodyBottom);
		Result.Notes.Add(TEXT("No pistol grip was found under the rear of the mesh, so the grip point is a rough guess. Move it to where the hand should sit."));
	}
	Result.LengthCm = static_cast<float>(Len);
	Result.GripFromRearCm = static_cast<float>(GripR.X - XMin);

	TMap<FName, FVector> Guess;
	Guess.Add(TEXT("Grip_R"), GripR);

	// Magazine.
	{
		// The next protrusion ahead of the chosen right hand (the run after the grip run, or the first run ahead of it).
		int32 FirstRun = GripRun != INDEX_NONE ? GripRun + 1 : Runs.Num();
		if (In.GripRule == EMantleWeaponGripRule::Proportion)
		{
			FirstRun = 0;
			while (FirstRun < Runs.Num() && RunCentreX(Runs[FirstRun]) <= GripR.X)
			{
				++FirstRun;
			}
		}
		for (int32 R = FirstRun; R < Runs.Num(); ++R)
		{
			if (RunWidth(Runs[R]) <= 0.25 * Len && RunCentreX(Runs[R]) - GripR.X <= 0.25 * Len)
			{
				const FRun& Run = Runs[R];
				Guess.Add(TEXT("Magazine"), FVector(RunCentreX(Run), 0.5 * (Run.MinY + Run.MaxY), BodyBottom - 0.5 * (BodyBottom - Run.LowestZ)));
				break;
			}
		}
	}

	// Muzzle: the tip at the bore height, the middle of the last 5%.
	{
		FBox Tip;
		SlabBox(Q, XMax - 0.05 * Len, XMax, Tip);
		const FVector Centre = Tip.GetCenter();
		Guess.Add(TEXT("Muzzle"), FVector(XMax, Centre.Y, Centre.Z));
	}

	// Sight: the top surface of the slab 35% to 45% from the rear.
	{
		FBox Slab;
		if (SlabBox(Q, XMin + 0.35 * Len, XMin + 0.45 * Len, Slab))
		{
			FBox Top(ForceInit);
			for (const FVector& P : Q)
			{
				if (P.X >= XMin + 0.35 * Len && P.X <= XMin + 0.45 * Len && P.Z >= Slab.Max.Z - 0.02 * Len)
				{
					Top += P;
				}
			}
			Guess.Add(TEXT("Sight"), FVector(XMin + 0.4 * Len, Top.GetCenter().Y, Slab.Max.Z));
			Result.Notes.Add(TEXT("The sight point is a guess at the top of the gun. Check that it sits where the player should look along the barrel."));
		}
	}

	// Grip_L for two-handed holds: 40% of the length ahead of the right hand, kept 10% of the length short of the
	// muzzle, under the middle of whatever the mesh has there (the handguard) by LeftHandBelowCentreCm.
	if (IsTwoHanded(In.Hold))
	{
		const double ForeX = FMath::Max(FMath::Min(GripR.X + 0.4 * Len, XMax - 0.1 * Len), GripR.X + 0.05 * Len);
		FBox Fore;
		if (SlabBox(Q, ForeX - 0.02 * Len, ForeX + 0.02 * Len, Fore))
		{
			Guess.Add(TEXT("Grip_L"), FVector(ForeX, Fore.GetCenter().Y, Fore.GetCenter().Z - In.LeftHandBelowCentreCm));
		}
		else
		{
			Guess.Add(TEXT("Grip_L"), FVector(ForeX, GripR.Y, GripR.Z));
		}
		Result.Notes.Add(TEXT("The left hand is placed ahead of the right on the front of the gun. Move it if it should hold elsewhere."));
	}

	// Grip_L for one-handed holds is only a guess for the support hand, as an offset from Grip_R in the weapon frame (X along
	// the bore, Y to the right, Z up, so the left side is -Y). The values come from the hand-tuned pack WK-11 hold.
	if (!IsTwoHanded(In.Hold))
	{
		static const FVector SupportHandOffsetCm(1.6, -2.6, -2.9);
		Guess.Add(TEXT("Grip_L"), GripR + SupportHandOffsetCm);
		Result.Notes.Add(TEXT("The left hand is a guess beside the grip. Move it if the support hand should sit elsewhere."));
	}

	// 7. The transform: p_weapon = Scale * Frame * (p_mesh - Centroid) - GripR. Rows of the matrix are where each mesh
	//    axis goes; a proper rotation by construction (Y = Z cross X).
	const FMatrix Frame(
		FVector(AxisX.X, AxisY.X, AxisZ.X),
		FVector(AxisX.Y, AxisY.Y, AxisZ.Y),
		FVector(AxisX.Z, AxisY.Z, AxisZ.Z),
		FVector::ZeroVector);
	const FVector CentroidInFrame(Centroid | AxisX, Centroid | AxisY, Centroid | AxisZ);
	Result.MeshToWeapon = FTransform(FQuat(Frame), -(CentroidInFrame * Scale) - GripR, FVector(Scale));

	for (const TPair<FName, FVector>& Pair : Guess)
	{
		Result.Points.Add(Pair.Key, FTransform(FQuat::Identity, Pair.Value - GripR));
	}
	return Result;
}
