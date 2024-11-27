/**
 * @Author: Kasper de Bruin bruinkasper@gmail.com
 * @Date: 2024-11-27 11:49:14
 * @LastEditors: Kasper de Bruin bruinkasper@gmail.com
 * @LastEditTime: 2024-11-27 13:13:51
 * @FilePath: Plugins/Gameplay/ThirdParty/ALS/Source/ALS/Private/AnimInstance/Als_TS_AnimInstance.cpp
 * @Description: Function implementations of ALSAnimInstance That Are Thread Safe
 */

#include "AlsAnimationInstance.h"

#include "AlsAnimationInstanceProxy.h"
#include "AlsCharacter.h"
#include "DrawDebugHelpers.h"
#include "Curves/CurveFloat.h"
#include "Engine/SkeletalMesh.h"
#include "Settings/AlsAnimationInstanceSettings.h"
#include "Utility/AlsConstants.h"
#include "Utility/AlsDebugUtility.h"
#include "Utility/AlsMacros.h"
#include "Utility/AlsPrivateMemberAccessor.h"
#include "Utility/AlsRotation.h"
#include "Utility/AlsUtility.h"

#include "Utility/AlsVector.h"

ALS_DEFINE_PRIVATE_MEMBER_ACCESSOR(AlsGetAnimationCurvesAccessor, &FAnimInstanceProxy::GetAnimationCurves,
	const TMap<FName, float>& (FAnimInstanceProxy::*) (EAnimCurveType) const)

void UAlsAnimationInstance::NativeThreadSafeUpdateAnimation(const float DeltaTime)
{
	DECLARE_SCOPE_CYCLE_COUNTER(TEXT("UAlsAnimationInstance::NativeThreadSafeUpdateAnimation"),
		STAT_UAlsAnimationInstance_NativeThreadSafeUpdateAnimation, STATGROUP_Als)
	TRACE_CPUPROFILER_EVENT_SCOPE(__FUNCTION__);

	Super::NativeThreadSafeUpdateAnimation(DeltaTime);

	if (!IsValid(Settings) || !IsValid(Character))
	{
		return;
	}

	DynamicTransitionsState.bUpdatedThisFrame = false;
	RotateInPlaceState.bUpdatedThisFrame      = false;
	TurnInPlaceState.bUpdatedThisFrame        = false;

	TS_Refresh(DeltaTime);
}

#pragma region Native Thread Safe Operation Functions

void UAlsAnimationInstance::TS_Refresh(const float DeltaTime)
{
	TS_RefreshLayering();
	TS_RefreshPose();
	TS_RefreshView(DeltaTime);
	TS_RefreshFeet(DeltaTime);
	TS_RefreshTransitions();
}

void UAlsAnimationInstance::TS_RefreshLayering()
{
	const auto& Curves{AlsGetAnimationCurvesAccessor::Access(GetProxyOnAnyThread<FAnimInstanceProxy>(), EAnimCurveType::AttributeCurve)};

	static const auto GetCurveValue{[](const TMap<FName, float>& Curves, const FName& CurveName) -> float
		{
			const auto* Value{Curves.Find(CurveName)};

			return Value != nullptr ? *Value : 0.0f;
		}};

	LayeringState.HeadBlendAmount         = GetCurveValue(Curves, UAlsConstants::LayerHeadCurveName());
	LayeringState.HeadAdditiveBlendAmount = GetCurveValue(Curves, UAlsConstants::LayerHeadAdditiveCurveName());
	LayeringState.HeadSlotBlendAmount     = GetCurveValue(Curves, UAlsConstants::LayerHeadSlotCurveName());

	// The mesh space blend will always be 1 unless the local space blend is 1.

	LayeringState.ArmLeftBlendAmount           = GetCurveValue(Curves, UAlsConstants::LayerArmLeftCurveName());
	LayeringState.ArmLeftAdditiveBlendAmount   = GetCurveValue(Curves, UAlsConstants::LayerArmLeftAdditiveCurveName());
	LayeringState.ArmLeftSlotBlendAmount       = GetCurveValue(Curves, UAlsConstants::LayerArmLeftSlotCurveName());
	LayeringState.ArmLeftLocalSpaceBlendAmount = GetCurveValue(Curves, UAlsConstants::LayerArmLeftLocalSpaceCurveName());
	LayeringState.ArmLeftMeshSpaceBlendAmount  = !FAnimWeight::IsFullWeight(LayeringState.ArmLeftLocalSpaceBlendAmount);

	// The mesh space blend will always be 1 unless the local space blend is 1.

	LayeringState.ArmRightBlendAmount           = GetCurveValue(Curves, UAlsConstants::LayerArmRightCurveName());
	LayeringState.ArmRightAdditiveBlendAmount   = GetCurveValue(Curves, UAlsConstants::LayerArmRightAdditiveCurveName());
	LayeringState.ArmRightSlotBlendAmount       = GetCurveValue(Curves, UAlsConstants::LayerArmRightSlotCurveName());
	LayeringState.ArmRightLocalSpaceBlendAmount = GetCurveValue(Curves, UAlsConstants::LayerArmRightLocalSpaceCurveName());
	LayeringState.ArmRightMeshSpaceBlendAmount  = !FAnimWeight::IsFullWeight(LayeringState.ArmRightLocalSpaceBlendAmount);

	LayeringState.HandLeftBlendAmount  = GetCurveValue(Curves, UAlsConstants::LayerHandLeftCurveName());
	LayeringState.HandRightBlendAmount = GetCurveValue(Curves, UAlsConstants::LayerHandRightCurveName());

	LayeringState.SpineBlendAmount         = GetCurveValue(Curves, UAlsConstants::LayerSpineCurveName());
	LayeringState.SpineAdditiveBlendAmount = GetCurveValue(Curves, UAlsConstants::LayerSpineAdditiveCurveName());
	LayeringState.SpineSlotBlendAmount     = GetCurveValue(Curves, UAlsConstants::LayerSpineSlotCurveName());

	LayeringState.PelvisBlendAmount     = GetCurveValue(Curves, UAlsConstants::LayerPelvisCurveName());
	LayeringState.PelvisSlotBlendAmount = GetCurveValue(Curves, UAlsConstants::LayerPelvisSlotCurveName());

	LayeringState.LegsBlendAmount     = GetCurveValue(Curves, UAlsConstants::LayerLegsCurveName());
	LayeringState.LegsSlotBlendAmount = GetCurveValue(Curves, UAlsConstants::LayerLegsSlotCurveName());
}

void UAlsAnimationInstance::TS_RefreshPose()
{
	const auto& Curves{AlsGetAnimationCurvesAccessor::Access(GetProxyOnAnyThread<FAnimInstanceProxy>(), EAnimCurveType::AttributeCurve)};

	static const auto GetCurveValue{[](const TMap<FName, float>& Curves, const FName& CurveName) -> float
		{
			const auto* Value{Curves.Find(CurveName)};

			return Value != nullptr ? *Value : 0.0f;
		}};

	PoseState.GroundedAmount = GetCurveValue(Curves, UAlsConstants::PoseGroundedCurveName());
	PoseState.InAirAmount    = GetCurveValue(Curves, UAlsConstants::PoseInAirCurveName());

	PoseState.StandingAmount  = GetCurveValue(Curves, UAlsConstants::PoseStandingCurveName());
	PoseState.CrouchingAmount = GetCurveValue(Curves, UAlsConstants::PoseCrouchingCurveName());

	PoseState.MovingAmount = GetCurveValue(Curves, UAlsConstants::PoseMovingCurveName());

	PoseState.GaitAmount          = FMath::Clamp(GetCurveValue(Curves, UAlsConstants::PoseGaitCurveName()), 0.0f, 3.0f);
	PoseState.GaitWalkingAmount   = UAlsMath::Clamp01(PoseState.GaitAmount);
	PoseState.GaitRunningAmount   = UAlsMath::Clamp01(PoseState.GaitAmount - 1.0f);
	PoseState.GaitSprintingAmount = UAlsMath::Clamp01(PoseState.GaitAmount - 2.0f);

	// Use the grounded pose curve value to "unweight" the gait pose curve. This is used to
	// instantly get the full gait value from the very beginning of transitions to grounded states.

	PoseState.UnweightedGaitAmount =
		PoseState.GroundedAmount > UE_SMALL_NUMBER ? PoseState.GaitAmount / PoseState.GroundedAmount : PoseState.GaitAmount;

	PoseState.UnweightedGaitWalkingAmount   = UAlsMath::Clamp01(PoseState.UnweightedGaitAmount);
	PoseState.UnweightedGaitRunningAmount   = UAlsMath::Clamp01(PoseState.UnweightedGaitAmount - 1.0f);
	PoseState.UnweightedGaitSprintingAmount = UAlsMath::Clamp01(PoseState.UnweightedGaitAmount - 2.0f);
}

void UAlsAnimationInstance::TS_RefreshView(const float DeltaTime)
{
	if (!LocomotionAction.IsValid())
	{
		ViewState.YawAngle   = FMath::UnwindDegrees(UE_REAL_TO_FLOAT(ViewState.Rotation.Yaw - LocomotionState.Rotation.Yaw));
		ViewState.PitchAngle = FMath::UnwindDegrees(UE_REAL_TO_FLOAT(ViewState.Rotation.Pitch - LocomotionState.Rotation.Pitch));

		ViewState.PitchAmount = 0.5f - ViewState.PitchAngle / 180.0f;
	}

	const auto ViewAmount{1.0f - GetCurveValueClamped01(UAlsConstants::ViewBlockCurveName())};
	const auto AimingAmount{GetCurveValueClamped01(UAlsConstants::AllowAimingCurveName())};

	ViewState.LookAmount = ViewAmount * (1.0f - AimingAmount);

	RefreshSpine(ViewAmount * AimingAmount, DeltaTime);
}

void UAlsAnimationInstance::TS_RefreshFeet(const float DeltaTime)
{
	FeetState.FootPlantedAmount  = FMath::Clamp(GetCurveValue(UAlsConstants::FootPlantedCurveName()), -1.0f, 1.0f);
	FeetState.FeetCrossingAmount = GetCurveValueClamped01(UAlsConstants::FeetCrossingCurveName());

	const auto ComponentTransformInverse{GetProxyOnAnyThread<FAnimInstanceProxy>().GetComponentTransform().Inverse()};

	RefreshFoot(
		FeetState.Left, UAlsConstants::FootLeftIkCurveName(), UAlsConstants::FootLeftLockCurveName(), ComponentTransformInverse, DeltaTime);

	RefreshFoot(FeetState.Right, UAlsConstants::FootRightIkCurveName(), UAlsConstants::FootRightLockCurveName(), ComponentTransformInverse,
		DeltaTime);
}

void UAlsAnimationInstance::TS_RefreshTransitions()
{
	// The allow transitions curve is modified within certain states, so that transitions allowed will be true while in those states.
	TransitionsState.bTransitionsAllowed = FAnimWeight::IsFullWeight(GetCurveValue(UAlsConstants::AllowTransitionsCurveName()));
}

#pragma endregion

void UAlsAnimationInstance::RefreshFoot(FAlsFootState& FootState, const FName& IkCurveName, const FName& LockCurveName,
	const FTransform& ComponentTransformInverse, const float DeltaTime) const
{
	const auto IkAmount{GetCurveValueClamped01(IkCurveName)};

	ProcessFootLockTeleport(IkAmount, FootState);
	ProcessFootLockBaseChange(IkAmount, FootState, ComponentTransformInverse);
	RefreshFootLock(IkAmount, FootState, LockCurveName, ComponentTransformInverse, DeltaTime);
}

/*UFUNTION*/
void UAlsAnimationInstance::RefreshDynamicTransitions()
{
#if WITH_EDITOR
	if (!IsValid(GetWorld()) || !GetWorld()->IsGameWorld())
	{
		return;
	}
#endif

	DECLARE_SCOPE_CYCLE_COUNTER(
		TEXT("UAlsAnimationInstance::RefreshDynamicTransitions"), STAT_UAlsAnimationInstance_RefreshDynamicTransitions, STATGROUP_Als)
	TRACE_CPUPROFILER_EVENT_SCOPE(__FUNCTION__);

	if (DynamicTransitionsState.bUpdatedThisFrame || !IsValid(Settings))
	{
		return;
	}

	DynamicTransitionsState.bUpdatedThisFrame = true;

	if (DynamicTransitionsState.FrameDelay > 0)
	{
		DynamicTransitionsState.FrameDelay -= 1;
		return;
	}

	if (!TransitionsState.bTransitionsAllowed)
	{
		return;
	}

	// Check each foot to see if the location difference between the foot look and its desired / target location
	// exceeds a threshold. If it does, play an additive transition animation on that foot. The currently set
	// transition plays the second half of a 2 foot transition animation, so that only a single foot moves.

	const auto FootLockDistanceThresholdSquared{
		FMath::Square(Settings->DynamicTransitions.FootLockDistanceThreshold * LocomotionState.Scale)};

	const auto FootLockLeftDistanceSquared{FVector::DistSquared(FeetState.Left.TargetLocation, FeetState.Left.LockLocation)};
	const auto FootLockRightDistanceSquared{FVector::DistSquared(FeetState.Right.TargetLocation, FeetState.Right.LockLocation)};

	const auto bTransitionLeftAllowed{
		FAnimWeight::IsRelevant(FeetState.Left.LockAmount) && FootLockLeftDistanceSquared > FootLockDistanceThresholdSquared};

	const auto bTransitionRightAllowed{
		FAnimWeight::IsRelevant(FeetState.Right.LockAmount) && FootLockRightDistanceSquared > FootLockDistanceThresholdSquared};

	if (!bTransitionLeftAllowed && !bTransitionRightAllowed)
	{
		return;
	}

	TObjectPtr<UAnimSequenceBase> DynamicTransitionSequence;

	// If both transitions are allowed, choose the one with a greater lock distance.

	if (!bTransitionLeftAllowed)
	{
		DynamicTransitionSequence = Stance == AlsStanceTags::Crouching ? Settings->DynamicTransitions.CrouchingRightSequence
		                                                               : Settings->DynamicTransitions.StandingRightSequence;
	}
	else if (!bTransitionRightAllowed)
	{
		DynamicTransitionSequence = Stance == AlsStanceTags::Crouching ? Settings->DynamicTransitions.CrouchingLeftSequence
		                                                               : Settings->DynamicTransitions.StandingLeftSequence;
	}
	else if (FootLockLeftDistanceSquared >= FootLockRightDistanceSquared)
	{
		DynamicTransitionSequence = Stance == AlsStanceTags::Crouching ? Settings->DynamicTransitions.CrouchingLeftSequence
		                                                               : Settings->DynamicTransitions.StandingLeftSequence;
	}
	else
	{
		DynamicTransitionSequence = Stance == AlsStanceTags::Crouching ? Settings->DynamicTransitions.CrouchingRightSequence
		                                                               : Settings->DynamicTransitions.StandingRightSequence;
	}

	if (IsValid(DynamicTransitionSequence))
	{
		// Block next dynamic transitions for about 2 frames to give the animation blueprint some time to properly react to the animation.

		DynamicTransitionsState.FrameDelay = 2;

		// Animation montages can't be played in the worker thread, so queue them up to play later in the game thread.

		TransitionsState.QueuedTransitionSequence         = DynamicTransitionSequence;
		TransitionsState.QueuedTransitionBlendInDuration  = Settings->DynamicTransitions.BlendDuration;
		TransitionsState.QueuedTransitionBlendOutDuration = Settings->DynamicTransitions.BlendDuration;
		TransitionsState.QueuedTransitionPlayRate         = Settings->DynamicTransitions.PlayRate;
		TransitionsState.QueuedTransitionStartTime        = 0.0f;

		if (IsInGameThread())
		{
			PlayQueuedTransitionAnimation();
		}
	}
}
/*UFUNTION*/
void UAlsAnimationInstance::RefreshRotateInPlace()
{
#if WITH_EDITOR
	if (!IsValid(GetWorld()) || !GetWorld()->IsGameWorld())
	{
		return;
	}
#endif

	DECLARE_SCOPE_CYCLE_COUNTER(
		TEXT("UAlsAnimationInstance::RefreshRotateInPlace"), STAT_UAlsAnimationInstance_RefreshRotateInPlace, STATGROUP_Als)
	TRACE_CPUPROFILER_EVENT_SCOPE(__FUNCTION__);

	if (RotateInPlaceState.bUpdatedThisFrame || !IsValid(Settings))
	{
		return;
	}

	RotateInPlaceState.bUpdatedThisFrame = true;

	if (LocomotionState.bMoving || !IsRotateInPlaceAllowed())
	{
		RotateInPlaceState.bRotatingLeft  = false;
		RotateInPlaceState.bRotatingRight = false;
	}
	else
	{
		// Check if the character should rotate left or right by checking if the view yaw angle exceeds the threshold.

		RotateInPlaceState.bRotatingLeft  = ViewState.YawAngle < -Settings->RotateInPlace.ViewYawAngleThreshold;
		RotateInPlaceState.bRotatingRight = ViewState.YawAngle > Settings->RotateInPlace.ViewYawAngleThreshold;
	}

	static constexpr auto PlayRateInterpolationSpeed{5.0f};

	if (!RotateInPlaceState.bRotatingLeft && !RotateInPlaceState.bRotatingRight)
	{
		RotateInPlaceState.PlayRate = bPendingUpdate ? Settings->RotateInPlace.PlayRate.X
		                                             : FMath::FInterpTo(RotateInPlaceState.PlayRate, Settings->RotateInPlace.PlayRate.X,
														   GetDeltaSeconds(), PlayRateInterpolationSpeed);
		return;
	}

	// If the character should rotate, set the play rate to scale with the view yaw
	// speed. This makes the character rotate faster when moving the camera faster.

	const auto PlayRate{FMath::GetMappedRangeValueClamped(
		Settings->RotateInPlace.ReferenceViewYawSpeed, Settings->RotateInPlace.PlayRate, ViewState.YawSpeed)};

	RotateInPlaceState.PlayRate =
		bPendingUpdate ? PlayRate : FMath::FInterpTo(RotateInPlaceState.PlayRate, PlayRate, GetDeltaSeconds(), PlayRateInterpolationSpeed);
}
/*UFUNTION*/
void UAlsAnimationInstance::RefreshTurnInPlace()
{
#if WITH_EDITOR
	if (!IsValid(GetWorld()) || !GetWorld()->IsGameWorld())
	{
		return;
	}
#endif

	DECLARE_SCOPE_CYCLE_COUNTER(
		TEXT("UAlsAnimationInstance::RefreshTurnInPlace"), STAT_UAlsAnimationInstance_RefreshTurnInPlace, STATGROUP_Als)
	TRACE_CPUPROFILER_EVENT_SCOPE(__FUNCTION__);

	if (TurnInPlaceState.bUpdatedThisFrame || !IsValid(Settings))
	{
		return;
	}

	TurnInPlaceState.bUpdatedThisFrame = true;

	if (!TransitionsState.bTransitionsAllowed || !IsTurnInPlaceAllowed())
	{
		TurnInPlaceState.ActivationDelay = 0.0f;
		return;
	}

	// Check if the view yaw speed is below the threshold and if the view yaw angle is outside the
	// threshold. If so, begin counting the activation delay time. If not, reset the activation delay
	// time. This ensures the conditions remain true for a sustained time before turning in place.

	if (ViewState.YawSpeed >= Settings->TurnInPlace.ViewYawSpeedThreshold ||
		FMath::Abs(ViewState.YawAngle) <= Settings->TurnInPlace.ViewYawAngleThreshold)
	{
		TurnInPlaceState.ActivationDelay = 0.0f;
		return;
	}

	TurnInPlaceState.ActivationDelay = TurnInPlaceState.ActivationDelay + GetDeltaSeconds();

	const auto ActivationDelay{FMath::GetMappedRangeValueClamped({Settings->TurnInPlace.ViewYawAngleThreshold, 180.0f},
		Settings->TurnInPlace.ViewYawAngleToActivationDelay, FMath::Abs(ViewState.YawAngle))};

	// Check if the activation delay time exceeds the set delay (mapped to the view yaw angle). If so, start a turn in place.

	if (TurnInPlaceState.ActivationDelay <= ActivationDelay)
	{
		return;
	}

	// Select settings based on turn angle and stance.

	const auto bTurnLeft{UAlsRotation::RemapAngleForCounterClockwiseRotation(ViewState.YawAngle) <= 0.0f};

	UAlsTurnInPlaceSettings* TurnInPlaceSettings{nullptr};
	FName                    TurnInPlaceSlotName;

	if (Stance == AlsStanceTags::Standing)
	{
		TurnInPlaceSlotName = UAlsConstants::TurnInPlaceStandingSlotName();

		if (FMath::Abs(ViewState.YawAngle) < Settings->TurnInPlace.Turn180AngleThreshold)
		{
			TurnInPlaceSettings = bTurnLeft ? Settings->TurnInPlace.StandingTurn90Left : Settings->TurnInPlace.StandingTurn90Right;
		}
		else
		{
			TurnInPlaceSettings = bTurnLeft ? Settings->TurnInPlace.StandingTurn180Left : Settings->TurnInPlace.StandingTurn180Right;
		}
	}
	else if (Stance == AlsStanceTags::Crouching)
	{
		TurnInPlaceSlotName = UAlsConstants::TurnInPlaceCrouchingSlotName();

		if (FMath::Abs(ViewState.YawAngle) < Settings->TurnInPlace.Turn180AngleThreshold)
		{
			TurnInPlaceSettings = bTurnLeft ? Settings->TurnInPlace.CrouchingTurn90Left : Settings->TurnInPlace.CrouchingTurn90Right;
		}
		else
		{
			TurnInPlaceSettings = bTurnLeft ? Settings->TurnInPlace.CrouchingTurn180Left : Settings->TurnInPlace.CrouchingTurn180Right;
		}
	}

	if (IsValid(TurnInPlaceSettings) && ALS_ENSURE(IsValid(TurnInPlaceSettings->Sequence)))
	{
		// Animation montages can't be played in the worker thread, so queue them up to play later in the game thread.

		TurnInPlaceState.QueuedSettings     = TurnInPlaceSettings;
		TurnInPlaceState.QueuedSlotName     = TurnInPlaceSlotName;
		TurnInPlaceState.QueuedTurnYawAngle = ViewState.YawAngle;

		if (IsInGameThread())
		{
			PlayQueuedTurnInPlaceAnimation();
		}
	}
}
/*UFUNTION*/
void UAlsAnimationInstance::RefreshInAir()
{
#if WITH_EDITOR
	if (!IsValid(GetWorld()) || !GetWorld()->IsGameWorld())
	{
		return;
	}
#endif

	DECLARE_SCOPE_CYCLE_COUNTER(TEXT("UAlsAnimationInstance::RefreshInAir"), STAT_UAlsAnimationInstance_RefreshInAir, STATGROUP_Als)
	TRACE_CPUPROFILER_EVENT_SCOPE(__FUNCTION__);

	if (!IsValid(Settings))
	{
		return;
	}

	if (InAirState.bJumped)
	{
		static constexpr auto ReferenceSpeed{600.0f};
		static constexpr auto MinPlayRate{1.2f};
		static constexpr auto MaxPlayRate{1.5f};

		InAirState.bJumped      = false;
		InAirState.JumpPlayRate = UAlsMath::LerpClamped(MinPlayRate, MaxPlayRate, LocomotionState.Speed / ReferenceSpeed);
	}

	// A separate variable for vertical speed is used to determine at what speed the character landed on the ground.

	InAirState.VerticalVelocity = UE_REAL_TO_FLOAT(LocomotionState.Velocity.Z);

	RefreshGroundPrediction();
	RefreshInAirLean();
}

/*Gets Called from the ufunction*/
void UAlsAnimationInstance::RefreshGroundPrediction()
{
	// Calculate the ground prediction weight by tracing in the velocity direction to find a walkable surface the character
	// is falling toward and getting the "time" (range from 0 to 1, 1 being maximum, 0 being about to ground) till impact.
	// The ground prediction amount curve is used to control how the time affects the final amount for a smooth blend.

	static constexpr auto VerticalVelocityThreshold{-200.0f};

	if (InAirState.VerticalVelocity > VerticalVelocityThreshold)
	{
		InAirState.GroundPredictionAmount = 0.0f;
		return;
	}

	const auto AllowanceAmount{1.0f - GetCurveValueClamped01(UAlsConstants::GroundPredictionBlockCurveName())};
	if (AllowanceAmount <= UE_KINDA_SMALL_NUMBER)
	{
		InAirState.GroundPredictionAmount = 0.0f;
		return;
	}

	const auto SweepStartLocation{LocomotionState.Location};

	static constexpr auto MinVerticalVelocity{-4000.0f};
	static constexpr auto MaxVerticalVelocity{-200.0f};

	auto VelocityDirection{LocomotionState.Velocity};
	VelocityDirection.Z = FMath::Clamp(VelocityDirection.Z, MinVerticalVelocity, MaxVerticalVelocity);
	VelocityDirection.Normalize();

	static constexpr auto MinSweepDistance{150.0f};
	static constexpr auto MaxSweepDistance{2000.0f};

	const auto SweepVector{VelocityDirection *
						   FMath::GetMappedRangeValueClamped(FVector2f{MaxVerticalVelocity, MinVerticalVelocity},
							   {MinSweepDistance, MaxSweepDistance}, InAirState.VerticalVelocity) *
						   LocomotionState.Scale};

	FHitResult Hit;
	GetWorld()->SweepSingleByChannel(Hit, SweepStartLocation, SweepStartLocation + SweepVector, FQuat::Identity,
		Settings->InAir.GroundPredictionSweepChannel,
		FCollisionShape::MakeCapsule(LocomotionState.CapsuleRadius, LocomotionState.CapsuleHalfHeight), {__FUNCTION__, false, Character},
		Settings->InAir.GroundPredictionSweepResponses);

	const auto bGroundValid{Hit.IsValidBlockingHit() && Hit.ImpactNormal.Z >= LocomotionState.WalkableFloorAngleCos};

#if WITH_EDITORONLY_DATA && ENABLE_DRAW_DEBUG
	if (bDisplayDebugTraces)
	{
		if (IsInGameThread())
		{
			UAlsDebugUtility::DrawSweepSingleCapsule(GetWorld(), Hit.TraceStart, Hit.TraceEnd, FRotator::ZeroRotator,
				LocomotionState.CapsuleRadius, LocomotionState.CapsuleHalfHeight, bGroundValid, Hit, {0.25f, 0.0f, 1.0f},
				{0.75f, 0.0f, 1.0f});
		}
		else
		{
			DisplayDebugTracesQueue.Emplace(
				[this, Hit, bGroundValid]
				{
					UAlsDebugUtility::DrawSweepSingleCapsule(GetWorld(), Hit.TraceStart, Hit.TraceEnd, FRotator::ZeroRotator,
						LocomotionState.CapsuleRadius, LocomotionState.CapsuleHalfHeight, bGroundValid, Hit, {0.25f, 0.0f, 1.0f},
						{0.75f, 0.0f, 1.0f});
				});
		}
	}
#endif

	InAirState.GroundPredictionAmount =
		bGroundValid ? Settings->InAir.GroundPredictionAmountCurve->GetFloatValue(Hit.Time) * AllowanceAmount : 0.0f;
}

/*Gets Called from the ufunction*/
void UAlsAnimationInstance::RefreshInAirLean()
{
	// Use the relative velocity direction and amount to determine how much the character should lean
	// while in air. The lean amount curve gets the vertical velocity and is used as a multiplier to
	// smoothly reverse the leaning direction when transitioning from moving upwards to moving downwards.

	static constexpr auto ReferenceSpeed{350.0f};

	const auto TargetLeanAmount{
		GetRelativeVelocity() / ReferenceSpeed * Settings->InAir.LeanAmountCurve->GetFloatValue(InAirState.VerticalVelocity)};

	if (bPendingUpdate || Settings->General.LeanInterpolationSpeed <= 0.0f)
	{
		LeanState.RightAmount   = TargetLeanAmount.Y;
		LeanState.ForwardAmount = TargetLeanAmount.X;
	}
	else
	{
		const auto InterpolationAmount{UAlsMath::ExponentialDecay(GetDeltaSeconds(), Settings->General.LeanInterpolationSpeed)};

		LeanState.RightAmount   = FMath::Lerp(LeanState.RightAmount, TargetLeanAmount.Y, InterpolationAmount);
		LeanState.ForwardAmount = FMath::Lerp(LeanState.ForwardAmount, TargetLeanAmount.X, InterpolationAmount);
	}
}

/*UFUNCTION*/
void UAlsAnimationInstance::RefreshGrounded()
{
#if WITH_EDITOR
	if (!IsValid(GetWorld()) || !GetWorld()->IsGameWorld())
	{
		return;
	}
#endif

	DECLARE_SCOPE_CYCLE_COUNTER(TEXT("UAlsAnimationInstance::RefreshGrounded"), STAT_UAlsAnimationInstance_RefreshGrounded, STATGROUP_Als)
	TRACE_CPUPROFILER_EVENT_SCOPE(__FUNCTION__);

	if (!IsValid(Settings))
	{
		return;
	}

	RefreshVelocityBlend();
	RefreshGroundedLean();
}

/*UFUNCTION*/
void UAlsAnimationInstance::RefreshLook()
{
#if WITH_EDITOR
	if (!IsValid(GetWorld()) || !GetWorld()->IsGameWorld())
	{
		return;
	}
#endif

	DECLARE_SCOPE_CYCLE_COUNTER(TEXT("UAlsAnimationInstance::RefreshLook"), STAT_UAlsAnimationInstance_RefreshLook, STATGROUP_Als)
	TRACE_CPUPROFILER_EVENT_SCOPE(__FUNCTION__);

	if (!IsValid(Settings))
	{
		return;
	}

	const auto ActorYawAngle{UE_REAL_TO_FLOAT(LocomotionState.Rotation.Yaw)};

	if (MovementBase.bHasRelativeRotation)
	{
		// Offset the angle to keep it relative to the movement base.
		LookState.WorldYawAngle = FMath::UnwindDegrees(UE_REAL_TO_FLOAT(LookState.WorldYawAngle + MovementBase.DeltaRotation.Yaw));
	}

	float TargetYawAngle;
	float TargetPitchAngle;
	float InterpolationSpeed;

	if (RotationMode == AlsRotationModeTags::VelocityDirection)
	{
		// Look towards input direction.

		TargetYawAngle = FMath::UnwindDegrees(
			(LocomotionState.bHasInput ? LocomotionState.InputYawAngle : LocomotionState.TargetYawAngle) - ActorYawAngle);

		TargetPitchAngle   = 0.0f;
		InterpolationSpeed = Settings->View.LookTowardsInputYawAngleInterpolationSpeed;
	}
	else
	{
		// Look towards view direction.

		TargetYawAngle     = ViewState.YawAngle;
		TargetPitchAngle   = ViewState.PitchAngle;
		InterpolationSpeed = Settings->View.LookTowardsCameraRotationInterpolationSpeed;
	}

	if (LookState.bInitializationRequired || InterpolationSpeed <= 0.0f)
	{
		LookState.YawAngle   = TargetYawAngle;
		LookState.PitchAngle = TargetPitchAngle;

		LookState.bInitializationRequired = false;
	}
	else
	{
		const auto YawAngle{FMath::UnwindDegrees(LookState.WorldYawAngle - ActorYawAngle)};
		auto       DeltaYawAngle{FMath::UnwindDegrees(TargetYawAngle - YawAngle)};

		if (DeltaYawAngle > 180.0f - UAlsRotation::CounterClockwiseRotationAngleThreshold)
		{
			DeltaYawAngle -= 360.0f;
		}
		else if (FMath::Abs(LocomotionState.YawSpeed) > UE_SMALL_NUMBER && FMath::Abs(TargetYawAngle) > 90.0f)
		{
			// When interpolating yaw angle, favor the character rotation direction, over the shortest rotation
			// direction, so that the rotation of the head remains synchronized with the rotation of the body.

			DeltaYawAngle = LocomotionState.YawSpeed > 0.0f ? FMath::Abs(DeltaYawAngle) : -FMath::Abs(DeltaYawAngle);
		}

		const auto InterpolationAmount{UAlsMath::ExponentialDecay(GetDeltaSeconds(), InterpolationSpeed)};

		LookState.YawAngle   = FMath::UnwindDegrees(YawAngle + DeltaYawAngle * InterpolationAmount);
		LookState.PitchAngle = UAlsRotation::LerpAngle(LookState.PitchAngle, TargetPitchAngle, InterpolationAmount);
	}

	LookState.WorldYawAngle = FMath::UnwindDegrees(ActorYawAngle + LookState.YawAngle);

	// Separate the yaw angle into 3 separate values. These 3 values are used to improve the
	// blending of the view when rotating completely around the character. This allows to
	// keep the view responsive but still smoothly blend from left to right or right to left.

	LookState.YawForwardAmount = LookState.YawAngle / 360.0f + 0.5f;
	LookState.YawLeftAmount    = 0.5f - FMath::Abs(LookState.YawForwardAmount - 0.5f);
	LookState.YawRightAmount   = 0.5f + FMath::Abs(LookState.YawForwardAmount - 0.5f);
}