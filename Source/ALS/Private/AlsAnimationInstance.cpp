#include "AlsAnimationInstance.h"

#include "AlsAnimationInstanceProxy.h"
#include "AlsCharacter.h"
#include "DrawDebugHelpers.h"
#include "Curves/CurveFloat.h"
#include "Engine/SkeletalMesh.h"
#include "Settings/AlsAnimationInstanceSettings.h"
#include "Utility/AlsConstants.h"
#include "Utility/AlsMacros.h"
#include "Utility/AlsRotation.h"
#include "Utility/AlsUtility.h"
#include "Utility/AlsVector.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(AlsAnimationInstance)

void UAlsAnimationInstance::NativeInitializeAnimation()
{
	Super::NativeInitializeAnimation();

	Character = Cast<AAlsCharacter>(GetOwningActor());

#if WITH_EDITOR
	const auto* World{GetWorld()};

	if (IsValid(World) && !World->IsGameWorld() && !IsValid(Character))
	{
		// Use default objects for editor preview.
		Character = GetMutableDefault<AAlsCharacter>();
	}
#endif

	const auto* Mesh{GetSkelMeshComponent()};

	if (IsValid(Mesh) && IsValid(Mesh->GetSkinnedAsset()))
	{
		const auto& ReferenceSkeleton{Mesh->GetSkinnedAsset()->GetRefSkeleton()};
		const auto  PelvisBoneIndex{ReferenceSkeleton.FindBoneIndex(UAlsConstants::PelvisBoneName())};

		static const auto GetThighAxis{
			[](const FReferenceSkeleton& ReferenceSkeleton, const int32 PelvisBoneIndex, const FName& FootBoneName, FVector3f& ThighAxis)
			{
				auto ParentBoneIndex{ReferenceSkeleton.FindBoneIndex(FootBoneName)};
				if (ParentBoneIndex < 0)
				{
					return;
				}

				while (true)
				{
					const auto NextParentBoneIndex{ReferenceSkeleton.GetParentIndex(ParentBoneIndex)};
					if (NextParentBoneIndex <= 0)
					{
						return;
					}

					if (NextParentBoneIndex == PelvisBoneIndex)
					{
						break;
					}

					ParentBoneIndex = NextParentBoneIndex;
				}

				const auto& ThighTransform{ReferenceSkeleton.GetRefBonePose()[ParentBoneIndex]};

				ThighAxis = FVector3f{ThighTransform.GetLocation()};
				ThighAxis.Normalize();
			}};

		GetThighAxis(ReferenceSkeleton, PelvisBoneIndex, UAlsConstants::FootLeftBoneName(), FeetState.Left.ThighAxis);
		GetThighAxis(ReferenceSkeleton, PelvisBoneIndex, UAlsConstants::FootRightBoneName(), FeetState.Right.ThighAxis);
	}
}

void UAlsAnimationInstance::NativeBeginPlay()
{
	Super::NativeBeginPlay();

	ALS_ENSURE(IsValid(Settings));
	ALS_ENSURE(IsValid(Character));
}

void UAlsAnimationInstance::NativePostUpdateAnimation()
{
	// Can't use UAnimationInstance::NativePostEvaluateAnimation() instead this function, as it will not be called if
	// USkinnedMeshComponent::VisibilityBasedAnimTickOption is set to EVisibilityBasedAnimTickOption::AlwaysTickPose.

	DECLARE_SCOPE_CYCLE_COUNTER(
		TEXT("UAlsAnimationInstance::NativePostUpdateAnimation"), STAT_UAlsAnimationInstance_NativePostUpdateAnimation, STATGROUP_Als)
	TRACE_CPUPROFILER_EVENT_SCOPE(__FUNCTION__);

	if (!IsValid(Settings) || !IsValid(Character))
	{
		return;
	}

	PlayQueuedTransitionAnimation();
	PlayQueuedTurnInPlaceAnimation();
	StopQueuedTransitionAndTurnInPlaceAnimations();

#if WITH_EDITORONLY_DATA && ENABLE_DRAW_DEBUG
	if (!bPendingUpdate)
	{
		for (const auto& DisplayDebugTraceFunction : DisplayDebugTracesQueue)
		{
			DisplayDebugTraceFunction();
		}
	}

	DisplayDebugTracesQueue.Reset();
#endif

	bPendingUpdate = false;
}

FAnimInstanceProxy* UAlsAnimationInstance::CreateAnimInstanceProxy()
{
	return new FAlsAnimationInstanceProxy{this};
}

FAlsControlRigInput UAlsAnimationInstance::TS_NativeGetControlRigInput() const
{
	return {.bUseHandIkBones         = !IsValid(Settings) || Settings->General.bUseHandIkBones,
		.bUseFootIkBones             = !IsValid(Settings) || Settings->General.bUseFootIkBones,
		.bFootOffsetAllowed          = LocomotionMode != AlsLocomotionModeTags::InAir,
		.VelocityBlendForwardAmount  = GroundedState.VelocityBlend.ForwardAmount,
		.VelocityBlendBackwardAmount = GroundedState.VelocityBlend.BackwardAmount,
		.FootLeftLocation{FVector{FeetState.Left.FinalLocation}},
		.FootLeftRotation{FQuat{FeetState.Left.FinalRotation}},
		.FootRightLocation{FVector{FeetState.Right.FinalLocation}},
		.FootRightRotation{FQuat{FeetState.Right.FinalRotation}},
		.SpineYawAngle = SpineState.YawAngle};
}

FAlsControlRigInput UAlsAnimationInstance::GetControlRigInput() const
{
	return TS_NativeGetControlRigInput();
}

bool UAlsAnimationInstance::IsSpineRotationAllowed()
{
	return RotationMode == AlsRotationModeTags::Aiming;
}

void UAlsAnimationInstance::RefreshSpine(const float SpineBlendAmount, const float DeltaTime)
{
	if (SpineState.bSpineRotationAllowed != IsSpineRotationAllowed())
	{
		SpineState.bSpineRotationAllowed = !SpineState.bSpineRotationAllowed;

		if (SpineState.bSpineRotationAllowed)
		{
			// Remap SpineAmount from the [SpineAmount, 1] range to [0, 1] so that lerp between new LastYawAngle
			// and ViewState.YawAngle with an alpha equal to SpineAmount still results in CurrentYawAngle.

			if (FAnimWeight::IsFullWeight(SpineState.SpineAmount))
			{
				SpineState.SpineAmountScale = 1.0f;
				SpineState.SpineAmountBias  = 0.0f;
			}
			else
			{
				SpineState.SpineAmountScale = 1.0f / (1.0f - SpineState.SpineAmount);
				SpineState.SpineAmountBias  = -SpineState.SpineAmount * SpineState.SpineAmountScale;
			}
		}
		else
		{
			// Remap SpineAmount from the [0, SpineAmount] range to [0, 1] so that lerp between 0
			// and LastYawAngle with an alpha equal to SpineAmount still results in CurrentYawAngle.

			SpineState.SpineAmountScale = !FAnimWeight::IsRelevant(SpineState.SpineAmount) ? 1.0f : 1.0f / SpineState.SpineAmount;

			SpineState.SpineAmountBias = 0.0f;
		}

		SpineState.LastYawAngle      = SpineState.CurrentYawAngle;
		SpineState.LastActorYawAngle = UE_REAL_TO_FLOAT(LocomotionState.Rotation.Yaw);
	}

	if (SpineState.bSpineRotationAllowed)
	{
		if (bPendingUpdate || FAnimWeight::IsFullWeight(SpineState.SpineAmount))
		{
			SpineState.SpineAmount     = 1.0f;
			SpineState.CurrentYawAngle = ViewState.YawAngle;
		}
		else
		{
			static constexpr auto InterpolationSpeed{20.0f};

			SpineState.SpineAmount = UAlsMath::ExponentialDecay(SpineState.SpineAmount, 1.0f, DeltaTime, InterpolationSpeed);

			SpineState.CurrentYawAngle = UAlsRotation::LerpAngle(SpineState.LastYawAngle, ViewState.YawAngle,
				SpineState.SpineAmount * SpineState.SpineAmountScale + SpineState.SpineAmountBias);
		}
	}
	else
	{
		if (bPendingUpdate || !FAnimWeight::IsRelevant(SpineState.SpineAmount))
		{
			SpineState.SpineAmount     = 0.0f;
			SpineState.CurrentYawAngle = 0.0f;
		}
		else
		{
			static constexpr auto InterpolationSpeed{1.0f};
			static constexpr auto ReferenceViewYawSpeed{40.0f};

			// Increase the interpolation speed when the camera rotates quickly,
			// otherwise the spine rotation may lag too much behind the actor rotation.

			const auto InterpolationSpeedMultiplier{FMath::Max(1.0f, FMath::Abs(ViewState.YawSpeed) / ReferenceViewYawSpeed)};

			SpineState.SpineAmount =
				UAlsMath::ExponentialDecay(SpineState.SpineAmount, 0.0f, DeltaTime, InterpolationSpeed * InterpolationSpeedMultiplier);

			if (MovementBase.bHasRelativeRotation)
			{
				// Offset the angle to keep it relative to the movement base.
				SpineState.LastActorYawAngle =
					FMath::UnwindDegrees(UE_REAL_TO_FLOAT(SpineState.LastActorYawAngle + MovementBase.DeltaRotation.Yaw));
			}

			// Offset the spine rotation to keep it unchanged in world space to achieve a smoother spine rotation when aiming stops.

			auto YawAngleOffset{FMath::UnwindDegrees(UE_REAL_TO_FLOAT(SpineState.LastActorYawAngle - LocomotionState.Rotation.Yaw))};

			// Keep the offset within 30 degrees, otherwise the spine rotation may lag too much behind the actor rotation.

			static constexpr auto MaxYawAngleOffset{30.0f};
			YawAngleOffset = FMath::Clamp(YawAngleOffset, -MaxYawAngleOffset, MaxYawAngleOffset);

			SpineState.LastActorYawAngle = FMath::UnwindDegrees(UE_REAL_TO_FLOAT(YawAngleOffset + LocomotionState.Rotation.Yaw));

			SpineState.CurrentYawAngle = UAlsRotation::LerpAngle(0.0f, SpineState.LastYawAngle + YawAngleOffset,
				SpineState.SpineAmount * SpineState.SpineAmountScale + SpineState.SpineAmountBias);
		}
	}

	SpineState.YawAngle = UAlsRotation::LerpAngle(0.0f, SpineState.CurrentYawAngle, SpineBlendAmount);
}

void UAlsAnimationInstance::InitializeLook()
{
	LookState.bInitializationRequired = true;
}

void UAlsAnimationInstance::InitializeLean()
{
	LeanState.RightAmount   = 0.0f;
	LeanState.ForwardAmount = 0.0f;
}

void UAlsAnimationInstance::InitializeGrounded()
{
	GroundedState.VelocityBlend.bInitializationRequired = true;
}

FVector3f UAlsAnimationInstance::GetRelativeVelocity() const
{
	return FVector3f{LocomotionState.RotationQuaternion.UnrotateVector(LocomotionState.Velocity)};
}

FVector2f UAlsAnimationInstance::GetRelativeAccelerationAmount() const
{
	// This value represents the current amount of acceleration / deceleration relative to the
	// character rotation. It is normalized to a range of -1 to 1 so that -1 equals the max
	// braking deceleration and 1 equals the max acceleration of the character movement component.

	const auto MaxAcceleration{(LocomotionState.Acceleration | LocomotionState.Velocity) >= 0.0f ? LocomotionState.MaxAcceleration
																								 : LocomotionState.MaxBrakingDeceleration};

	if (MaxAcceleration <= UE_KINDA_SMALL_NUMBER)
	{
		return FVector2f::ZeroVector;
	}

	const FVector3f RelativeAcceleration{LocomotionState.RotationQuaternion.UnrotateVector(LocomotionState.Acceleration)};

	return FVector2f{UAlsVector::ClampMagnitude01(RelativeAcceleration / MaxAcceleration)};
}

void UAlsAnimationInstance::RefreshVelocityBlend()
{
	// Calculate and interpolate the velocity blend amounts. This value represents the velocity amount of
	// the character in each direction (normalized so that diagonals equal 0.5 for each direction) and is
	// used in a blend multi node to produce better directional blending than a standard blend space.

	auto& VelocityBlend{GroundedState.VelocityBlend};

	auto RelativeVelocityDirection{GetRelativeVelocity()};
	auto TargetVelocityBlend{FVector3f::ZeroVector};

	if (RelativeVelocityDirection.Normalize())
	{
		TargetVelocityBlend =
			RelativeVelocityDirection /
			(FMath::Abs(RelativeVelocityDirection.X) + FMath::Abs(RelativeVelocityDirection.Y) + FMath::Abs(RelativeVelocityDirection.Z));
	}

	if (VelocityBlend.bInitializationRequired || Settings->Grounded.VelocityBlendInterpolationSpeed <= 0.0f)
	{
		VelocityBlend.bInitializationRequired = false;

		VelocityBlend.ForwardAmount  = UAlsMath::Clamp01(TargetVelocityBlend.X);
		VelocityBlend.BackwardAmount = FMath::Abs(FMath::Clamp(TargetVelocityBlend.X, -1.0f, 0.0f));
		VelocityBlend.LeftAmount     = FMath::Abs(FMath::Clamp(TargetVelocityBlend.Y, -1.0f, 0.0f));
		VelocityBlend.RightAmount    = UAlsMath::Clamp01(TargetVelocityBlend.Y);
	}
	else
	{
		// WWe use UAlsMath::ExponentialDecay() instead of FMath::FInterpTo(), because FMath::FInterpTo() is very sensitive to large
		// delta time, at low FPS interpolation becomes almost instant which causes issues with character pose during the stop.

		const auto InterpolationAmount{UAlsMath::ExponentialDecay(GetDeltaSeconds(), Settings->Grounded.VelocityBlendInterpolationSpeed)};

		VelocityBlend.ForwardAmount =
			FMath::Lerp(VelocityBlend.ForwardAmount, UAlsMath::Clamp01(TargetVelocityBlend.X), InterpolationAmount);

		VelocityBlend.BackwardAmount =
			FMath::Lerp(VelocityBlend.BackwardAmount, FMath::Abs(FMath::Clamp(TargetVelocityBlend.X, -1.0f, 0.0f)), InterpolationAmount);

		VelocityBlend.LeftAmount =
			FMath::Lerp(VelocityBlend.LeftAmount, FMath::Abs(FMath::Clamp(TargetVelocityBlend.Y, -1.0f, 0.0f)), InterpolationAmount);

		VelocityBlend.RightAmount = FMath::Lerp(VelocityBlend.RightAmount, UAlsMath::Clamp01(TargetVelocityBlend.Y), InterpolationAmount);
	}
}

void UAlsAnimationInstance::RefreshGroundedLean()
{
	const auto TargetLeanAmount{GetRelativeAccelerationAmount()};

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

void UAlsAnimationInstance::RefreshGroundedMovement()
{
#if WITH_EDITOR
	if (!IsValid(GetWorld()) || !GetWorld()->IsGameWorld())
	{
		return;
	}
#endif

	DECLARE_SCOPE_CYCLE_COUNTER(
		TEXT("UAlsAnimationInstance::RefreshGroundedMovement"), STAT_UAlsAnimationInstance_RefreshGroundedMovement, STATGROUP_Als)
	TRACE_CPUPROFILER_EVENT_SCOPE(__FUNCTION__);

	if (!IsValid(Settings))
	{
		return;
	}

	GroundedState.HipsDirectionLockAmount = FMath::Clamp(GetCurveValue(UAlsConstants::HipsDirectionLockCurveName()), -1.0f, 1.0f);

	const auto ViewRelativeVelocityYawAngle{
		FMath::UnwindDegrees(UE_REAL_TO_FLOAT(LocomotionState.VelocityYawAngle - ViewState.Rotation.Yaw))};

	RefreshMovementDirection(ViewRelativeVelocityYawAngle);
	RefreshRotationYawOffsets(ViewRelativeVelocityYawAngle);
}

void UAlsAnimationInstance::RefreshMovementDirection(const float ViewRelativeVelocityYawAngle)
{
	// Calculate the movement direction. This value represents the direction the character is moving relative to the camera during
	// the view direction and aiming rotation modes and is used in the cycle blending to blend to the appropriate directional states.

	if (RotationMode == AlsRotationModeTags::VelocityDirection || Gait == AlsGaitTags::Sprinting)
	{
		GroundedState.MovementDirection = EAlsMovementDirection::Forward;
		return;
	}

	static constexpr auto ForwardHalfAngle{70.0f};
	static constexpr auto AngleThreshold{5.0f};

	GroundedState.MovementDirection = UAlsMath::CalculateMovementDirection(ViewRelativeVelocityYawAngle, ForwardHalfAngle, AngleThreshold);
}

void UAlsAnimationInstance::RefreshRotationYawOffsets(const float ViewRelativeVelocityYawAngle)
{
	// Rotation yaw offsets influence the rotation yaw offset curve in the animation
	// graph and is used to offset the character's rotation for more natural movement.
	// The curves allow us to precisely control the offset for each movement direction.

	auto& RotationYawOffsets{GroundedState.RotationYawOffsets};

	RotationYawOffsets.ForwardAngle  = Settings->Grounded.RotationYawOffsetForwardCurve->GetFloatValue(ViewRelativeVelocityYawAngle);
	RotationYawOffsets.BackwardAngle = Settings->Grounded.RotationYawOffsetBackwardCurve->GetFloatValue(ViewRelativeVelocityYawAngle);
	RotationYawOffsets.LeftAngle     = Settings->Grounded.RotationYawOffsetLeftCurve->GetFloatValue(ViewRelativeVelocityYawAngle);
	RotationYawOffsets.RightAngle    = Settings->Grounded.RotationYawOffsetRightCurve->GetFloatValue(ViewRelativeVelocityYawAngle);
}

void UAlsAnimationInstance::InitializeStandingMovement()
{
	StandingState.SprintTime   = 0.0f;
	StandingState.bPivotActive = false;
}

void UAlsAnimationInstance::RefreshStandingMovement()
{
#if WITH_EDITOR
	if (!IsValid(GetWorld()) || !GetWorld()->IsGameWorld())
	{
		return;
	}
#endif

	DECLARE_SCOPE_CYCLE_COUNTER(
		TEXT("UAlsAnimationInstance::RefreshStandingMovement"), STAT_UAlsAnimationInstance_RefreshStandingMovement, STATGROUP_Als)
	TRACE_CPUPROFILER_EVENT_SCOPE(__FUNCTION__);

	if (!IsValid(Settings))
	{
		return;
	}

	const auto Speed{LocomotionState.Speed / LocomotionState.Scale};

	// Calculate the stride blend amount. This value is used within the blend spaces to scale the stride (distance feet travel)
	// so that the character can walk or run at different movement speeds. It also allows the walk or run gait animations to
	// blend independently while still matching the animation speed to the movement speed, preventing the character from needing
	// to play a half walk + half run blend. The curves are used to map the stride amount to the speed for maximum control.

	StandingState.StrideBlendAmount = FMath::Lerp(Settings->Standing.StrideBlendAmountWalkCurve->GetFloatValue(Speed),
		Settings->Standing.StrideBlendAmountRunCurve->GetFloatValue(Speed), PoseState.UnweightedGaitRunningAmount);

	// Calculate the walk run blend amount. This value is used within the blend spaces to blend between walking and running.

	StandingState.WalkRunBlendAmount = Gait == AlsGaitTags::Walking ? 0.0f : 1.0f;

	// Calculate the standing play rate by dividing the character's speed by the animated speed for each gait.
	// The interpolation is determined by the gait amount curve that exists on every locomotion cycle so that
	// the play rate is always in sync with the currently blended animation. The value is also divided by the
	// stride blend and the capsule scale so that the play rate increases as the stride or scale gets smaller.

	// TODO Automatically calculate the play rate, such as is done in the UAnimDistanceMatchingLibrary::SetPlayrateToMatchSpeed() function.

	const auto WalkRunSpeedAmount{FMath::Lerp(
		Speed / Settings->Standing.AnimatedWalkSpeed, Speed / Settings->Standing.AnimatedRunSpeed, PoseState.UnweightedGaitRunningAmount)};

	const auto WalkRunSprintSpeedAmount{
		FMath::Lerp(WalkRunSpeedAmount, Speed / Settings->Standing.AnimatedSprintSpeed, PoseState.UnweightedGaitSprintingAmount)};

	// Do not let the play rate be exactly zero, otherwise animation notifies
	// may start to be triggered every frame until the play rate is changed.
	// TODO Check the need for this hack in future engine versions.

	StandingState.PlayRate = FMath::Clamp(WalkRunSprintSpeedAmount / StandingState.StrideBlendAmount, UE_KINDA_SMALL_NUMBER, 3.0f);

	StandingState.SprintBlockAmount = GetCurveValueClamped01(UAlsConstants::SprintBlockCurveName());

	if (Gait != AlsGaitTags::Sprinting)
	{
		StandingState.SprintTime               = 0.0f;
		StandingState.SprintAccelerationAmount = 0.0f;
		return;
	}

	// Use the relative acceleration as the sprint relative acceleration if less than 0.5 seconds has
	// elapsed since the start of the sprint, otherwise set the sprint relative acceleration to zero.
	// This is necessary to apply the acceleration animation only at the beginning of the sprint.

	static constexpr auto SprintTimeThreshold{0.5f};

	StandingState.SprintTime = bPendingUpdate ? SprintTimeThreshold : StandingState.SprintTime + GetDeltaSeconds();

	StandingState.SprintAccelerationAmount = StandingState.SprintTime >= SprintTimeThreshold ? 0.0f : GetRelativeAccelerationAmount().X;
}

void UAlsAnimationInstance::ActivatePivot()
{
	StandingState.bPivotActive = LocomotionState.Speed < Settings->Standing.PivotActivationSpeedThreshold;
}

void UAlsAnimationInstance::RefreshCrouchingMovement()
{
#if WITH_EDITOR
	if (!IsValid(GetWorld()) || !GetWorld()->IsGameWorld())
	{
		return;
	}
#endif

	DECLARE_SCOPE_CYCLE_COUNTER(
		TEXT("UAlsAnimationInstance::RefreshCrouchingMovement"), STAT_UAlsAnimationInstance_RefreshCrouchingMovement, STATGROUP_Als)
	TRACE_CPUPROFILER_EVENT_SCOPE(__FUNCTION__);

	if (!IsValid(Settings))
	{
		return;
	}

	const auto Speed{LocomotionState.Speed / LocomotionState.Scale};

	CrouchingState.StrideBlendAmount = Settings->Crouching.StrideBlendAmountCurve->GetFloatValue(Speed);

	CrouchingState.PlayRate =
		FMath::Clamp(Speed / (Settings->Crouching.AnimatedCrouchSpeed * CrouchingState.StrideBlendAmount), UE_KINDA_SMALL_NUMBER, 2.0f);
}

void UAlsAnimationInstance::ProcessFootLockTeleport(const float IkAmount, FAlsFootState& FootState) const
{
	// Due to network smoothing, we assume that teleportation occurs over a short period of time, not
	// in one frame, since after accepting the teleportation event, the character can still be moved for
	// some indefinite time, and this must be taken into account in order to avoid foot lock glitches.

	if (bPendingUpdate || GetWorld()->TimeSince(TeleportedTime) > 0.2f || !FAnimWeight::IsRelevant(IkAmount * FootState.LockAmount))
	{
		return;
	}

	const auto& ComponentTransform{GetProxyOnAnyThread<FAnimInstanceProxy>().GetComponentTransform()};

	FootState.LockLocation = ComponentTransform.TransformPosition(FVector{FootState.LockComponentRelativeLocation});
	FootState.LockRotation = ComponentTransform.TransformRotation(FQuat{FootState.LockComponentRelativeRotation});

	if (MovementBase.bHasRelativeLocation)
	{
		const auto BaseRotationInverse{MovementBase.Rotation.Inverse()};

		FootState.LockMovementBaseRelativeLocation =
			FVector3f{BaseRotationInverse.RotateVector(FootState.LockLocation - MovementBase.Location)};

		FootState.LockMovementBaseRelativeRotation = FQuat4f{BaseRotationInverse * FootState.LockRotation};
	}
}

void UAlsAnimationInstance::ProcessFootLockBaseChange(
	const float IkAmount, FAlsFootState& FootState, const FTransform& ComponentTransformInverse) const
{
	if ((!bPendingUpdate && !MovementBase.bBaseChanged) || !FAnimWeight::IsRelevant(IkAmount * FootState.LockAmount))
	{
		return;
	}

	if (bPendingUpdate)
	{
		FootState.LockLocation = FootState.TargetLocation;
		FootState.LockRotation = FootState.TargetRotation;
	}

	FootState.LockComponentRelativeLocation = FVector3f{ComponentTransformInverse.TransformPosition(FootState.LockLocation)};
	FootState.LockComponentRelativeRotation = FQuat4f{ComponentTransformInverse.TransformRotation(FootState.LockRotation)};

	if (MovementBase.bHasRelativeLocation)
	{
		const auto BaseRotationInverse{MovementBase.Rotation.Inverse()};

		FootState.LockMovementBaseRelativeLocation =
			FVector3f{BaseRotationInverse.RotateVector(FootState.LockLocation - MovementBase.Location)};

		FootState.LockMovementBaseRelativeRotation = FQuat4f{BaseRotationInverse * FootState.LockRotation};
	}
	else
	{
		FootState.LockMovementBaseRelativeLocation = FVector3f::ZeroVector;
		FootState.LockMovementBaseRelativeRotation = FQuat4f::Identity;
	}
}

void UAlsAnimationInstance::RefreshFootLock(const float IkAmount, FAlsFootState& FootState, const FName& LockCurveName,
	const FTransform& ComponentTransformInverse, const float DeltaTime) const
{
	auto NewLockAmount{GetCurveValueClamped01(LockCurveName)};

	if (LocomotionState.bMovingSmooth || LocomotionMode != AlsLocomotionModeTags::Grounded)
	{
		// Smoothly disable foot lock if the character is moving or in the air,
		// instead of relying on the curve value from the animation blueprint.

		static constexpr auto MovingDecreaseSpeed{5.0f};
		static constexpr auto NotGroundedDecreaseSpeed{0.6f};

		NewLockAmount =
			bPendingUpdate
				? 0.0f
				: FMath::Max(0.0f, FMath::Min(NewLockAmount,
									   FootState.LockAmount -
										   DeltaTime * (LocomotionState.bMovingSmooth ? MovingDecreaseSpeed : NotGroundedDecreaseSpeed)));
	}

	if (Settings->Feet.bDisableFootLock || !FAnimWeight::IsRelevant(IkAmount * NewLockAmount))
	{
		if (FootState.LockAmount > 0.0f)
		{
			FootState.LockAmount = 0.0f;

			FootState.LockLocation = FVector::ZeroVector;
			FootState.LockRotation = FQuat::Identity;

			FootState.LockComponentRelativeLocation = FVector3f::ZeroVector;
			FootState.LockComponentRelativeRotation = FQuat4f::Identity;

			FootState.LockMovementBaseRelativeLocation = FVector3f::ZeroVector;
			FootState.LockMovementBaseRelativeRotation = FQuat4f::Identity;
		}

		FootState.FinalLocation = FVector3f{ComponentTransformInverse.TransformPosition(FootState.TargetLocation)};
		FootState.FinalRotation = FQuat4f{ComponentTransformInverse.TransformRotation(FootState.TargetRotation)};
		return;
	}

	const auto bNewAmountEqualOne{FAnimWeight::IsFullWeight(NewLockAmount)};
	const auto bNewAmountGreaterThanPrevious{NewLockAmount > FootState.LockAmount};

	// Update the foot lock amount only if the new amount is less than the current amount or equal to 1. This
	// allows the foot to blend out from a locked location or lock to a new location, but never blend in.

	if (bNewAmountEqualOne)
	{
		if (bNewAmountGreaterThanPrevious)
		{
			// If the new foot lock amount is 1 and the previous amount is less than 1, then save the new foot lock location and rotation.

			if (FootState.LockAmount <= 0.9f)
			{
				// Keep the same lock location and rotation when the previous lock
				// amount is close to 1 to get rid of the foot "teleportation" issue.

				FootState.LockLocation = FootState.TargetLocation;
				FootState.LockRotation = FootState.TargetRotation;

				FootState.LockComponentRelativeLocation = FVector3f{ComponentTransformInverse.TransformPosition(FootState.LockLocation)};
				FootState.LockComponentRelativeRotation = FQuat4f{ComponentTransformInverse.TransformRotation(FootState.LockRotation)};
			}

			if (MovementBase.bHasRelativeLocation)
			{
				const auto BaseRotationInverse{MovementBase.Rotation.Inverse()};

				FootState.LockMovementBaseRelativeLocation =
					FVector3f{BaseRotationInverse.RotateVector(FootState.TargetLocation - MovementBase.Location)};

				FootState.LockMovementBaseRelativeRotation = FQuat4f{BaseRotationInverse * FootState.TargetRotation};
			}
			else
			{
				FootState.LockMovementBaseRelativeLocation = FVector3f::ZeroVector;
				FootState.LockMovementBaseRelativeRotation = FQuat4f::Identity;
			}
		}

		FootState.LockAmount = 1.0f;
	}
	else if (!bNewAmountGreaterThanPrevious)
	{
		FootState.LockAmount = NewLockAmount;
	}

	if (MovementBase.bHasRelativeLocation)
	{
		FootState.LockLocation =
			MovementBase.Location + MovementBase.Rotation.RotateVector(FVector{FootState.LockMovementBaseRelativeLocation});

		FootState.LockRotation = MovementBase.Rotation * FQuat{FootState.LockMovementBaseRelativeRotation};
	}

	FootState.LockComponentRelativeLocation = FVector3f{ComponentTransformInverse.TransformPosition(FootState.LockLocation)};
	FootState.LockComponentRelativeRotation = FQuat4f{ComponentTransformInverse.TransformRotation(FootState.LockRotation)};

	// Limit the foot lock location so that legs do not twist into a spiral when the actor rotates quickly.

	const auto ComponentRelativeThighAxis{FeetState.PelvisRotation.RotateVector(FootState.ThighAxis)};
	const auto LockAngle{UAlsVector::AngleBetweenSignedXY(ComponentRelativeThighAxis, FootState.LockComponentRelativeLocation)};

	if (FMath::Abs(LockAngle) > Settings->Feet.FootLockAngleLimit + UE_KINDA_SMALL_NUMBER)
	{
		const auto    ConstrainedLockAngle{FMath::Clamp(LockAngle, -Settings->Feet.FootLockAngleLimit, Settings->Feet.FootLockAngleLimit)};
		const FQuat4f OffsetRotation{FVector3f::UpVector, FMath::DegreesToRadians(ConstrainedLockAngle - LockAngle)};

		FootState.LockComponentRelativeLocation = OffsetRotation.RotateVector(FootState.LockComponentRelativeLocation);
		FootState.LockComponentRelativeRotation = OffsetRotation * FootState.LockComponentRelativeRotation;
		FootState.LockComponentRelativeRotation.Normalize();

		const auto& ComponentTransform{GetProxyOnAnyThread<FAnimInstanceProxy>().GetComponentTransform()};

		FootState.LockLocation = ComponentTransform.TransformPosition(FVector{FootState.LockComponentRelativeLocation});
		FootState.LockRotation = ComponentTransform.TransformRotation(FQuat{FootState.LockComponentRelativeRotation});

		if (MovementBase.bHasRelativeLocation)
		{
			const auto BaseRotationInverse{MovementBase.Rotation.Inverse()};

			FootState.LockMovementBaseRelativeLocation =
				FVector3f{BaseRotationInverse.RotateVector(FootState.LockLocation - MovementBase.Location)};

			FootState.LockMovementBaseRelativeRotation = FQuat4f{BaseRotationInverse * FootState.LockRotation};
		}
	}

	const auto FinalLocation{FMath::Lerp(FootState.TargetLocation, FootState.LockLocation, FootState.LockAmount)};

	auto FinalRotation{FQuat::FastLerp(FootState.TargetRotation, FootState.LockRotation, FootState.LockAmount)};
	FinalRotation.Normalize();

	FootState.FinalLocation = FVector3f{ComponentTransformInverse.TransformPosition(FinalLocation)};
	FootState.FinalRotation = FQuat4f{ComponentTransformInverse.TransformRotation(FinalRotation)};
}

void UAlsAnimationInstance::PlayQuickStopAnimation()
{
	if (!IsValid(Settings))
	{
		return;
	}

	if (RotationMode != AlsRotationModeTags::VelocityDirection)
	{
		PlayTransitionLeftAnimation(Settings->Transitions.QuickStopBlendInDuration, Settings->Transitions.QuickStopBlendOutDuration,
			Settings->Transitions.QuickStopPlayRate.X, Settings->Transitions.QuickStopStartTime);
		return;
	}

	auto RemainingYawAngle{FMath::UnwindDegrees(UE_REAL_TO_FLOAT(
		(LocomotionState.bHasInput ? LocomotionState.InputYawAngle : LocomotionState.TargetYawAngle) - LocomotionState.Rotation.Yaw))};

	RemainingYawAngle = UAlsRotation::RemapAngleForCounterClockwiseRotation(RemainingYawAngle);

	// Scale quick stop animation play rate based on how far the character
	// is going to rotate. At 180 degrees, the play rate will be maximal.

	if (RemainingYawAngle <= 0.0f)
	{
		PlayTransitionLeftAnimation(Settings->Transitions.QuickStopBlendInDuration, Settings->Transitions.QuickStopBlendOutDuration,
			FMath::Lerp(Settings->Transitions.QuickStopPlayRate.X, Settings->Transitions.QuickStopPlayRate.Y,
				FMath::Abs(RemainingYawAngle) / 180.0f),
			Settings->Transitions.QuickStopStartTime);
	}
	else
	{
		PlayTransitionRightAnimation(Settings->Transitions.QuickStopBlendInDuration, Settings->Transitions.QuickStopBlendOutDuration,
			FMath::Lerp(Settings->Transitions.QuickStopPlayRate.X, Settings->Transitions.QuickStopPlayRate.Y,
				FMath::Abs(RemainingYawAngle) / 180.0f),
			Settings->Transitions.QuickStopStartTime);
	}
}

void UAlsAnimationInstance::PlayTransitionAnimation(UAnimSequenceBase* Sequence, const float BlendInDuration, const float BlendOutDuration,
	const float PlayRate, const float StartTime, const bool bFromStandingIdleOnly)
{
	if (bFromStandingIdleOnly && (LocomotionState.bMoving || Stance != AlsStanceTags::Standing))
	{
		return;
	}

	// Animation montages can't be played in the worker thread, so queue them up to play later in the game thread.

	TransitionsState.QueuedTransitionSequence         = Sequence;
	TransitionsState.QueuedTransitionBlendInDuration  = BlendInDuration;
	TransitionsState.QueuedTransitionBlendOutDuration = BlendOutDuration;
	TransitionsState.QueuedTransitionPlayRate         = PlayRate;
	TransitionsState.QueuedTransitionStartTime        = StartTime;

	if (IsInGameThread())
	{
		PlayQueuedTransitionAnimation();
	}
}

void UAlsAnimationInstance::PlayTransitionLeftAnimation(const float BlendInDuration, const float BlendOutDuration, const float PlayRate,
	const float StartTime, const bool bFromStandingIdleOnly)
{
	if (!IsValid(Settings))
	{
		return;
	}

	PlayTransitionAnimation(
		Stance == AlsStanceTags::Crouching ? Settings->Transitions.CrouchingLeftSequence : Settings->Transitions.StandingLeftSequence,
		BlendInDuration, BlendOutDuration, PlayRate, StartTime, bFromStandingIdleOnly);
}

void UAlsAnimationInstance::PlayTransitionRightAnimation(const float BlendInDuration, const float BlendOutDuration, const float PlayRate,
	const float StartTime, const bool bFromStandingIdleOnly)
{
	if (!IsValid(Settings))
	{
		return;
	}

	PlayTransitionAnimation(
		Stance == AlsStanceTags::Crouching ? Settings->Transitions.CrouchingRightSequence : Settings->Transitions.StandingRightSequence,
		BlendInDuration, BlendOutDuration, PlayRate, StartTime, bFromStandingIdleOnly);
}

void UAlsAnimationInstance::StopTransitionAndTurnInPlaceAnimations(const float BlendOutDuration)
{
	TransitionsState.bStopTransitionsQueued                = true;
	TransitionsState.QueuedStopTransitionsBlendOutDuration = BlendOutDuration;

	if (IsInGameThread())
	{
		StopQueuedTransitionAndTurnInPlaceAnimations();
	}
}

void UAlsAnimationInstance::PlayQueuedTransitionAnimation()
{
	check(IsInGameThread())

		if (TransitionsState.bStopTransitionsQueued || !IsValid(TransitionsState.QueuedTransitionSequence))
	{
		return;
	}

	PlaySlotAnimationAsDynamicMontage(TransitionsState.QueuedTransitionSequence, UAlsConstants::TransitionSlotName(),
		TransitionsState.QueuedTransitionBlendInDuration, TransitionsState.QueuedTransitionBlendOutDuration,
		TransitionsState.QueuedTransitionPlayRate, 1, 0.0f, TransitionsState.QueuedTransitionStartTime);

	TransitionsState.QueuedTransitionSequence         = nullptr;
	TransitionsState.QueuedTransitionBlendInDuration  = 0.0f;
	TransitionsState.QueuedTransitionBlendOutDuration = 0.0f;
	TransitionsState.QueuedTransitionPlayRate         = 1.0f;
	TransitionsState.QueuedTransitionStartTime        = 0.0f;
}

void UAlsAnimationInstance::StopQueuedTransitionAndTurnInPlaceAnimations()
{
	check(IsInGameThread())

		if (!TransitionsState.bStopTransitionsQueued)
	{
		return;
	}

	StopSlotAnimation(TransitionsState.QueuedStopTransitionsBlendOutDuration, UAlsConstants::TransitionSlotName());
	StopSlotAnimation(TransitionsState.QueuedStopTransitionsBlendOutDuration, UAlsConstants::TurnInPlaceStandingSlotName());
	StopSlotAnimation(TransitionsState.QueuedStopTransitionsBlendOutDuration, UAlsConstants::TurnInPlaceCrouchingSlotName());

	TransitionsState.bStopTransitionsQueued                = false;
	TransitionsState.QueuedStopTransitionsBlendOutDuration = 0.0f;
}

bool UAlsAnimationInstance::IsRotateInPlaceAllowed()
{
	return RotationMode == AlsRotationModeTags::Aiming || ViewMode == AlsViewModeTags::FirstPerson;
}

bool UAlsAnimationInstance::IsTurnInPlaceAllowed()
{
	return RotationMode == AlsRotationModeTags::ViewDirection && ViewMode != AlsViewModeTags::FirstPerson;
}

void UAlsAnimationInstance::InitializeTurnInPlace()
{
	TurnInPlaceState.ActivationDelay = 0.0f;
}

void UAlsAnimationInstance::PlayQueuedTurnInPlaceAnimation()
{
	check(IsInGameThread())

		if (TransitionsState.bStopTransitionsQueued || !IsValid(TurnInPlaceState.QueuedSettings))
	{
		return;
	}

	const auto* TurnInPlaceSettings{TurnInPlaceState.QueuedSettings.Get()};

	PlaySlotAnimationAsDynamicMontage(TurnInPlaceSettings->Sequence, TurnInPlaceState.QueuedSlotName, Settings->TurnInPlace.BlendDuration,
		Settings->TurnInPlace.BlendDuration, TurnInPlaceSettings->PlayRate, 1, 0.0f);

	// Scale the rotation yaw delta (gets scaled in animation graph) to compensate for play rate and turn angle (if allowed).

	TurnInPlaceState.PlayRate = TurnInPlaceSettings->PlayRate;

	if (TurnInPlaceSettings->bScalePlayRateByAnimatedTurnAngle)
	{
		TurnInPlaceState.PlayRate *= FMath::Abs(TurnInPlaceState.QueuedTurnYawAngle / TurnInPlaceSettings->AnimatedTurnAngle);
	}

	TurnInPlaceState.QueuedSettings     = nullptr;
	TurnInPlaceState.QueuedSlotName     = NAME_None;
	TurnInPlaceState.QueuedTurnYawAngle = 0.0f;
}

FPoseSnapshot& UAlsAnimationInstance::SnapshotFinalRagdollPose()
{
	check(IsInGameThread())

		// Save a snapshot of the current ragdoll pose for use in animation graph to blend out of the ragdoll.

		SnapshotPose(RagdollingState.FinalRagdollPose);

	return RagdollingState.FinalRagdollPose;
}

float UAlsAnimationInstance::GetCurveValueClamped01(const FName& CurveName) const
{
	return UAlsMath::Clamp01(GetCurveValue(CurveName));
}
