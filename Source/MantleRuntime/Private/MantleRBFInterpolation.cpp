// Copyright Joshua Eger. All Rights Reserved.
// Derived from Epic Games code: UE 5.8 MeshResizing's RBFInterpolation.cpp and HairStrands' GroomBindingBuilder.cpp and GroomRBFDeformer.cpp. Epic's line stays below.
// Copyright Epic Games, Inc. All Rights Reserved.
// Vendored from UE 5.8: MeshResizing's Private/MeshResizing/RBFInterpolation.cpp, and from HairStrands'
// Private/GroomBindingBuilder.cpp (GroomBinding_RBFWeighting) and Private/GroomRBFDeformer.cpp (GetEntryCount).
// Only what Mantle calls is kept, the
// arithmetic term for term and in the same order: the weights and deformations must stay bit-identical.
// Mantle's changes: the names, explicit casts, and the groom
// builder's local-space option dropped (MeshResizing never set it, so its offset was always zero).

#include "MantleRBFInterpolation.h"

#include "Async/ParallelFor.h"
#include "Async/TaskGraphInterfaces.h"
#include "Tasks/Task.h"

// Eigen for the matrix inverse, included the way GroomBindingBuilder.cpp includes it.
#ifndef EIGEN_MPL2_ONLY
#define EIGEN_MPL2_ONLY
#endif

#if defined(_MSC_VER) && USING_CODE_ANALYSIS
#pragma warning(push)
#pragma warning(disable:6294) // Ill-defined for-loop:  initial condition does not satisfy test.  Loop body not executed.
#endif
PRAGMA_DEFAULT_VISIBILITY_START
THIRD_PARTY_INCLUDES_START
#include <Eigen/Core>
#include <Eigen/SVD>
THIRD_PARTY_INCLUDES_END
PRAGMA_DEFAULT_VISIBILITY_END
#if defined(_MSC_VER) && USING_CODE_ANALYSIS
#pragma warning(pop)
#endif

// Named, not anonymous: MantleRuntime builds as a unity build.
namespace MantleRBFInterpolationPrivate
{
	/** FGroomRBFDeformer::GetEntryCount (HAIR_RBF_ENTRY_COUNT): the samples and the four affine terms. */
	uint32 GetRBFEntryCount(const uint32 InSampleCount)
	{
		return InSampleCount + 4u;
	}

	/** GroomBinding_RBFWeighting::FPointsSampler: farthest-point sampling, starting at the first valid point. */
	struct FMantleRBFPointsSampler
	{
		FMantleRBFPointsSampler(TArray<bool>& ValidPoints, const FVector3f* PointPositions, const int32 NumSamples)
		{
			int32 NumPoints = 0;
			const int32 StartIndex = StartingPoint(ValidPoints, NumPoints);

			const int32 SamplesCount = FMath::Min(NumPoints, NumSamples);
			if (SamplesCount != 0)
			{
				SampleIndices.SetNum(SamplesCount);
				SampleIndices[0] = static_cast<uint32>(StartIndex);
				ValidPoints[StartIndex] = false;

				TArray<float> PointsDistance;
				PointsDistance.Init(MAX_FLT, ValidPoints.Num());

				for (int32 i = 1; i < SamplesCount; ++i)
				{
					FurthestPoint(ValidPoints.Num(), PointPositions, static_cast<uint32>(i), ValidPoints, PointsDistance);
				}
				BuildPositions(PointPositions);
			}
		}

		/** Build the sample position from the sample indices */
		void BuildPositions(const FVector3f* PointPositions)
		{
			const int32 NumSamples = SampleIndices.Num();
			SamplePositions.SetNum(NumSamples);
			for (int32 i = 0; i < NumSamples; ++i)
			{
				SamplePositions[i] = PointPositions[SampleIndices[i]];
			}
		}

		/** Compute the furthest point */
		void FurthestPoint(const int32 NumPoints, const FVector3f* PointPositions, const uint32 SampleIndex, TArray<bool>& ValidPoints, TArray<float>& PointsDistance)
		{
			float FurthestDistance = 0.0f;
			uint32 PointIndex = 0;
			for (int32 j = 0; j < NumPoints; ++j)
			{
				if (ValidPoints[j])
				{
					PointsDistance[j] = FMath::Min((PointPositions[SampleIndices[SampleIndex - 1]] - PointPositions[j]).Size(), PointsDistance[j]);
					if (PointsDistance[j] >= FurthestDistance)
					{
						PointIndex = static_cast<uint32>(j);
						FurthestDistance = PointsDistance[j];
					}
				}
			}
			ValidPoints[PointIndex] = false;
			SampleIndices[SampleIndex] = PointIndex;
		}

		/** Compute the starting point */
		int32 StartingPoint(const TArray<bool>& ValidPoints, int32& NumPoints) const
		{
			int32 StartIndex = -1;
			NumPoints = 0;
			for (int32 i = 0; i < ValidPoints.Num(); ++i)
			{
				if (ValidPoints[i])
				{
					++NumPoints;
					if (StartIndex == -1)
					{
						StartIndex = i;
					}
				}
			}
			return StartIndex;
		}

		/** List of sampled points */
		TArray<uint32> SampleIndices;

		/** List of sampled positions */
		TArray<FVector3f> SamplePositions;
	};

	/** GroomBinding_RBFWeighting::FWeightsBuilder: the multiquadric system with the affine terms, and its pseudo-inverse. */
	struct FMantleRBFWeightsBuilder
	{
		FMantleRBFWeightsBuilder(const uint32 NumRows, const uint32 NumColumns, const FVector3f* SourcePositions, const FVector3f* TargetPositions)
		{
			const uint32 PolyRows = GetRBFEntryCount(NumRows);
			const uint32 PolyColumns = GetRBFEntryCount(NumColumns);

			const uint32 SampleCount = NumRows;
			const uint32 EntryCount = GetRBFEntryCount(SampleCount);
			const uint32 WeightCount = EntryCount * EntryCount;

			MatrixEntries.Init(0.0f, static_cast<int32>(PolyRows * PolyColumns));
			InverseEntries.Init(0.0f, static_cast<int32>(PolyRows * PolyColumns));

			// Sanity check
			check(NumRows == NumColumns);
			check(static_cast<int32>(WeightCount) == MatrixEntries.Num());
			check(static_cast<int32>(WeightCount) == InverseEntries.Num());

			TArray<float>& LocalEntries = MatrixEntries;
			ParallelFor(static_cast<int32>(NumRows),
				[
					NumRows,
					NumColumns,
					PolyColumns,
					SourcePositions,
					TargetPositions,
					&LocalEntries
				] (const int32 Row)
				{
					const uint32 RowIndex = static_cast<uint32>(Row);
					int32 EntryIndex = static_cast<int32>(RowIndex * PolyColumns);
					for (uint32 j = 0; j < NumColumns; ++j)
					{
						const float FunctionScale = (SourcePositions[RowIndex] - TargetPositions[j]).Size();
						LocalEntries[EntryIndex++] = static_cast<float>(FMath::Sqrt(FunctionScale * FunctionScale + 1.0));
					}
					LocalEntries[EntryIndex++] = 1.0f;
					LocalEntries[EntryIndex++] = SourcePositions[RowIndex].X;
					LocalEntries[EntryIndex++] = SourcePositions[RowIndex].Y;
					LocalEntries[EntryIndex++] = SourcePositions[RowIndex].Z;

					EntryIndex = static_cast<int32>(NumRows * PolyColumns + RowIndex);
					LocalEntries[EntryIndex] = 1.0f;

					EntryIndex += static_cast<int32>(PolyColumns);
					LocalEntries[EntryIndex] = SourcePositions[RowIndex].X;

					EntryIndex += static_cast<int32>(PolyColumns);
					LocalEntries[EntryIndex] = SourcePositions[RowIndex].Y;

					EntryIndex += static_cast<int32>(PolyColumns);
					LocalEntries[EntryIndex] = SourcePositions[RowIndex].Z;
				});
			static constexpr float REGUL_VALUE = 1e-4f;
			int32 EntryIndex = static_cast<int32>(NumRows * PolyColumns + NumColumns);
			LocalEntries[EntryIndex] = REGUL_VALUE;

			EntryIndex += static_cast<int32>(PolyColumns) + 1;
			LocalEntries[EntryIndex] = REGUL_VALUE;

			EntryIndex += static_cast<int32>(PolyColumns) + 1;
			LocalEntries[EntryIndex] = REGUL_VALUE;

			EntryIndex += static_cast<int32>(PolyColumns) + 1;
			LocalEntries[EntryIndex] = REGUL_VALUE;

			ComputeWeights(PolyRows, PolyColumns);
		}

		/** Compute the weights by inverting the matrix*/
		void ComputeWeights(const uint32 NumRows, const uint32 NumColumns)
		{
			using EigenMatrix = Eigen::Map<Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>;
			EigenMatrix WeightsMatrix(MatrixEntries.GetData(), NumRows, NumColumns);
			EigenMatrix WeightsInverse(InverseEntries.GetData(), NumColumns, NumRows);

			auto MatrixSvd = WeightsMatrix.jacobiSvd(Eigen::ComputeThinU | Eigen::ComputeThinV);
			const Eigen::VectorXf& SingularValues = MatrixSvd.singularValues();
			if (SingularValues.size() > 0)
			{
				const float Tolerance = FLT_EPSILON * SingularValues.array().abs()(0);
				WeightsInverse = MatrixSvd.matrixV() * (SingularValues.array().abs() > Tolerance).select(
					SingularValues.array().inverse(), 0).matrix().asDiagonal() * MatrixSvd.matrixU().adjoint();
			}
		}

		/** Entries in the dense structure */
		TArray<float> MatrixEntries;

		/** Entries of the matrix inverse */
		TArray<float> InverseEntries;
	};

	/** RBFInterpolation.cpp's UpdateInterpolationData. */
	void UpdateInterpolationData(const FMantleRBFPointsSampler& PointsSampler, const FMantleRBFWeightsBuilder& InterpolationWeights, FMantleRBFInterpolationData& OutData)
	{
		OutData.SampleIndices.SetNumUninitialized(PointsSampler.SampleIndices.Num());
		for (int32 Index = 0; Index < PointsSampler.SampleIndices.Num(); ++Index)
		{
			OutData.SampleIndices[Index] = static_cast<int32>(PointsSampler.SampleIndices[Index]);
		}

		OutData.SampleRestPositions = PointsSampler.SamplePositions;
		OutData.InterpolationWeights = InterpolationWeights.InverseEntries;
	}

	/** RBFInterpolation.cpp's UpdateMeshSamples (from GroomRBFDeformer). Each entry is one serial sum, so the batching does not change the result. */
	void UpdateMeshSamples(const TArray<FVector3f>& TargetPositions, const FMantleRBFInterpolationData& InterpolationData, TArray<FVector3f>& OutSampleDeformations)
	{
		const int32 MaxSampleCount = TargetPositions.Num();
		check(MaxSampleCount == InterpolationData.SampleIndices.Num());
		check(MaxSampleCount == InterpolationData.SampleRestPositions.Num());

		const int32 EntryCount = static_cast<int32>(GetRBFEntryCount(static_cast<uint32>(MaxSampleCount)));
		OutSampleDeformations.SetNum(EntryCount);

		const int32 NumTasks = FMath::Max(FMath::Min(int32(FTaskGraphInterface::Get().GetNumWorkerThreads()), EntryCount), 1);
		constexpr int32 MinEntryByTask = 10;
		const int32 EntryByTask = FMath::Max(FMath::DivideAndRoundUp(EntryCount, NumTasks), MinEntryByTask);
		const int32 NumBatches = FMath::DivideAndRoundUp(EntryCount, EntryByTask);
		TArray<UE::Tasks::FTask> PendingTasks;
		PendingTasks.Reserve(NumBatches);

		for (int32 BatchIndex = 0; BatchIndex < NumBatches; BatchIndex++)
		{
			const int32 StartIndex = BatchIndex * EntryByTask;
			int32 EndIndex = (BatchIndex + 1) * EntryByTask;
			EndIndex = BatchIndex == NumBatches - 1 ? FMath::Min(EntryCount, EndIndex) : EndIndex;

			UE::Tasks::FTask PendingTask = UE::Tasks::Launch(UE_SOURCE_LOCATION, [StartIndex, EndIndex, EntryCount, MaxSampleCount, &OutSampleDeformations, &InterpolationData, &TargetPositions]
			{
				for (int32 SampleIndex = StartIndex; SampleIndex < EndIndex; ++SampleIndex)
				{
					int32 WeightsOffset = SampleIndex * EntryCount;
					FVector3f SampleDeformation(FVector3f::ZeroVector);
					for (int32 Index = 0; Index < MaxSampleCount; ++Index, ++WeightsOffset)
					{
						SampleDeformation += InterpolationData.InterpolationWeights[WeightsOffset] * (TargetPositions[Index] - InterpolationData.SampleRestPositions[Index]);
					}
					OutSampleDeformations[SampleIndex] = SampleDeformation;
				}
			});
			PendingTasks.Add(PendingTask);
		}
		UE::Tasks::Wait(PendingTasks);
	}
}

void FMantleRBFInterpolation::GenerateWeights(const TConstArrayView<FVector3f> SourcePositions, const int32 NumInterpolationPoints, FMantleRBFInterpolationData& OutData)
{
	using namespace MantleRBFInterpolationPrivate;

	// Consider all points to be valid for now
	TArray<bool> ValidPoints;
	ValidPoints.Init(true, SourcePositions.Num());

	FMantleRBFPointsSampler PointsSampler(ValidPoints, SourcePositions.GetData(), NumInterpolationPoints);
	const int32 SampleCount = PointsSampler.SamplePositions.Num();
	FMantleRBFWeightsBuilder InterpolationWeights(static_cast<uint32>(SampleCount), static_cast<uint32>(SampleCount),
		PointsSampler.SamplePositions.GetData(), PointsSampler.SamplePositions.GetData());
	UpdateInterpolationData(PointsSampler, InterpolationWeights, OutData);
}

void FMantleRBFInterpolation::GenerateMeshSamples(const TArray<FVector3f>& TargetPositions, const FMantleRBFInterpolationData& InterpolationData,
	TArray<FVector3f>& SampleDeformations)
{
	MantleRBFInterpolationPrivate::UpdateMeshSamples(TargetPositions, InterpolationData, SampleDeformations);
}
