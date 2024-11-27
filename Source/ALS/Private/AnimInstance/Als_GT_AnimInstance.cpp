/**
 * @Author: Kasper de Bruin bruinkasper@gmail.com
 * @Date: 2024-11-27 11:49:14
 * @LastEditors: Kasper de Bruin bruinkasper@gmail.com
 * @LastEditTime: 2024-11-27 12:15:41
 * @FilePath: Plugins/Gameplay/ThirdParty/ALS/Source/ALS/Private/AnimInstance/Als_GT_AnimInstance.cpp
 * @Description: Function implementations of ALSAnimInstance That Happen On The Game Thread
 */

#include "AlsAnimationInstance.h"

void UAlsAnimationInstance::NativeUpdateAnimation(const float DeltaTime)
{
	DECLARE_SCOPE_CYCLE_COUNTER(
		TEXT("UAlsAnimationInstance::NativeUpdateAnimation"), STAT_UAlsAnimationInstance_NativeUpdateAnimation, STATGROUP_Als)
	TRACE_CPUPROFILER_EVENT_SCOPE(__FUNCTION__);

	Super::NativeUpdateAnimation(DeltaTime);

	if (!IsValid(Settings) || !IsValid(Character))
	{
		return;
	}

	auto* Mesh{GetSkelMeshComponent()};

	if (Mesh->IsUsingAbsoluteRotation() && IsValid(Mesh->GetAttachParent()))
	{
		const auto& ParentTransform{Mesh->GetAttachParent()->GetComponentTransform()};

		// Manually synchronize mesh rotation with character rotation.

		Mesh->MoveComponent(FVector::ZeroVector, ParentTransform.GetRotation() * Character->GetBaseRotationOffset(), false);

		// Re-cache proxy transforms to match the modified mesh transform.

		const auto& Proxy{GetProxyOnGameThread<FAnimInstanceProxy>()};
		const_cast<FTransform&>(Proxy.GetComponentTransform())         = Mesh->GetComponentTransform();
		const_cast<FTransform&>(Proxy.GetComponentRelativeTransform()) = Mesh->GetRelativeTransform();
		const_cast<FTransform&>(Proxy.GetActorTransform())             = Character->GetActorTransform();
	}

#if WITH_EDITORONLY_DATA && ENABLE_DRAW_DEBUG
	bDisplayDebugTraces = UAlsDebugUtility::ShouldDisplayDebugForActor(Character, UAlsConstants::TracesDebugDisplayName());
#endif

	ViewMode       = Character->GetViewMode();
	LocomotionMode = Character->GetLocomotionMode();
	RotationMode   = Character->GetRotationMode();
	Stance         = Character->GetStance();
	Gait           = Character->GetGait();
	OverlayMode    = Character->GetOverlayMode();

	if (LocomotionAction != Character->GetLocomotionAction())
	{
		LocomotionAction = Character->GetLocomotionAction();
		ResetGroundedEntryMode();
	}

	const auto PreviousLocation{LocomotionState.Location};

	if (!bPendingUpdate && IsValid(Character->GetSettings()) &&
		FVector::DistSquared(PreviousLocation, LocomotionState.Location) >
			FMath::Square(Character->GetSettings()->TeleportDistanceThreshold))
	{
		MarkTeleported();
	}
}

void UAlsAnimationInstance::GT_Refresh(const float DeltaTime)
{
	check(IsInGameThread())

		RefreshMovementBaseOnGameThread();
	RefreshViewOnGameThread();
	RefreshLocomotionOnGameThread();
	RefreshInAirOnGameThread();
	RefreshFeetOnGameThread();
	RefreshRagdollingOnGameThread();
}

void UAlsAnimationInstance::RefreshMovementBaseOnGameThread()
{
	const auto& BasedMovement{Character->GetBasedMovement()};

	if (BasedMovement.MovementBase != MovementBase.Primitive || BasedMovement.BoneName != MovementBase.BoneName)
	{
		MovementBase.Primitive    = BasedMovement.MovementBase;
		MovementBase.BoneName     = BasedMovement.BoneName;
		MovementBase.bBaseChanged = true;
	}
	else
	{
		MovementBase.bBaseChanged = false;
	}

	MovementBase.bHasRelativeLocation = BasedMovement.HasRelativeLocation();
	MovementBase.bHasRelativeRotation = MovementBase.bHasRelativeLocation && BasedMovement.bRelativeRotation;

	const auto PreviousRotation{MovementBase.Rotation};

	MovementBaseUtility::GetMovementBaseTransform(
		BasedMovement.MovementBase, BasedMovement.BoneName, MovementBase.Location, MovementBase.Rotation);

	MovementBase.DeltaRotation = MovementBase.bHasRelativeLocation && !MovementBase.bBaseChanged
	                               ? (MovementBase.Rotation * PreviousRotation.Inverse()).Rotator()
	                               : FRotator::ZeroRotator;
}

void UAlsAnimationInstance::RefreshViewOnGameThread()
{
	check(IsInGameThread())

		const auto& View{Character->GetViewState()};

	ViewState.Rotation = View.Rotation;
	ViewState.YawSpeed = View.YawSpeed;
}

void UAlsAnimationInstance::RefreshLocomotionOnGameThread()
{
	check(IsInGameThread())

		const auto* World{GetWorld()};

	const auto ActorDeltaTime{IsValid(World) ? World->GetDeltaSeconds() * Character->CustomTimeDilation : 0.0f};
	const auto bCanCalculateRateOfChange{!bPendingUpdate && ActorDeltaTime > UE_SMALL_NUMBER};

	const auto& Locomotion{Character->GetLocomotionState()};

	LocomotionState.bHasInput     = Locomotion.bHasInput;
	LocomotionState.InputYawAngle = Locomotion.InputYawAngle;

	const auto PreviousVelocity{LocomotionState.Velocity};

	LocomotionState.Speed            = Locomotion.Speed;
	LocomotionState.Velocity         = Locomotion.Velocity;
	LocomotionState.VelocityYawAngle = Locomotion.VelocityYawAngle;

	LocomotionState.Acceleration =
		bCanCalculateRateOfChange ? (LocomotionState.Velocity - PreviousVelocity) / ActorDeltaTime : FVector::ZeroVector;

	const auto* Movement{Character->GetCharacterMovement()};

	LocomotionState.MaxAcceleration        = Movement->GetMaxAcceleration();
	LocomotionState.MaxBrakingDeceleration = Movement->GetMaxBrakingDeceleration();
	LocomotionState.WalkableFloorAngleCos  = Movement->GetWalkableFloorZ();

	LocomotionState.bMoving = Locomotion.bMoving;

	LocomotionState.bMovingSmooth =
		(Locomotion.bHasInput && Locomotion.bHasVelocity) || Locomotion.Speed > Settings->General.MovingSmoothSpeedThreshold;

	LocomotionState.TargetYawAngle = Locomotion.TargetYawAngle;

	const auto PreviousYawAngle{LocomotionState.Rotation.Yaw};

	const auto& Proxy{GetProxyOnGameThread<FAnimInstanceProxy>()};
	const auto& ActorTransform{Proxy.GetActorTransform()};
	const auto& MeshRelativeTransform{Proxy.GetComponentRelativeTransform()};

	static const auto* EnableListenServerSmoothingConsoleVariable{
		IConsoleManager::Get().FindConsoleVariable(TEXT("p.NetEnableListenServerSmoothing"))};
	check(EnableListenServerSmoothingConsoleVariable != nullptr)

		if (Movement->NetworkSmoothingMode == ENetworkSmoothingMode::Disabled ||
			(Character->GetLocalRole() != ROLE_SimulatedProxy &&
				!(Character->IsNetMode(NM_ListenServer) && EnableListenServerSmoothingConsoleVariable->GetBool())))
	{
		// If the network smoothing is disabled, use the regular actor transform.

		LocomotionState.Location           = ActorTransform.GetLocation();
		LocomotionState.Rotation           = ActorTransform.Rotator();
		LocomotionState.RotationQuaternion = ActorTransform.GetRotation();
	}
	else if (GetSkelMeshComponent()->IsUsingAbsoluteRotation())
	{
		LocomotionState.Location =
			ActorTransform.TransformPosition(MeshRelativeTransform.GetLocation() - Character->GetBaseTranslationOffset());

		LocomotionState.Rotation           = ActorTransform.Rotator();
		LocomotionState.RotationQuaternion = ActorTransform.GetRotation();
	}
	else
	{
		const auto SmoothTransform{
			ActorTransform * FTransform{MeshRelativeTransform.GetRotation() * Character->GetBaseRotationOffset().Inverse(),
								 MeshRelativeTransform.GetLocation() - Character->GetBaseTranslationOffset()}};

		LocomotionState.Location           = SmoothTransform.GetLocation();
		LocomotionState.Rotation           = SmoothTransform.Rotator();
		LocomotionState.RotationQuaternion = SmoothTransform.GetRotation();
	}

	LocomotionState.YawSpeed = bCanCalculateRateOfChange
	                             ? FMath::UnwindDegrees(UE_REAL_TO_FLOAT(LocomotionState.Rotation.Yaw - PreviousYawAngle)) / ActorDeltaTime
	                             : 0.0f;

	LocomotionState.Scale = UE_REAL_TO_FLOAT(Proxy.GetComponentTransform().GetScale3D().Z);

	const auto* Capsule{Character->GetCapsuleComponent()};

	LocomotionState.CapsuleRadius     = Capsule->GetScaledCapsuleRadius();
	LocomotionState.CapsuleHalfHeight = Capsule->GetScaledCapsuleHalfHeight();
}
void UAlsAnimationInstance::RefreshFeetOnGameThread()
{
	check(IsInGameThread())

		const auto* Mesh{GetSkelMeshComponent()};

	FeetState.PelvisRotation = FQuat4f{Mesh->GetSocketTransform(UAlsConstants::PelvisBoneName(), RTS_Component).GetRotation()};

	const auto FootLeftTargetTransform{Mesh->GetSocketTransform(
		Settings->General.bUseFootIkBones ? UAlsConstants::FootLeftIkBoneName() : UAlsConstants::FootLeftVirtualBoneName())};

	FeetState.Left.TargetLocation = FootLeftTargetTransform.GetLocation();
	FeetState.Left.TargetRotation = FootLeftTargetTransform.GetRotation();

	const auto FootRightTargetTransform{Mesh->GetSocketTransform(
		Settings->General.bUseFootIkBones ? UAlsConstants::FootRightIkBoneName() : UAlsConstants::FootRightVirtualBoneName())};

	FeetState.Right.TargetLocation = FootRightTargetTransform.GetLocation();
	FeetState.Right.TargetRotation = FootRightTargetTransform.GetRotation();
}

void UAlsAnimationInstance::RefreshRagdollingOnGameThread()
{
	check(IsInGameThread())

		if (LocomotionAction != AlsLocomotionActionTags::Ragdolling)
	{
		return;
	}

	// Scale the flail play rate by the root speed. The faster the ragdoll moves, the faster the character will flail.

	static constexpr auto ReferenceSpeed{1000.0f};

	RagdollingState.FlailPlayRate = UAlsMath::Clamp01(UE_REAL_TO_FLOAT(Character->GetRagdollingState().Velocity.Size() / ReferenceSpeed));
}

void UAlsAnimationInstance::RefreshInAirOnGameThread()
{
	check(IsInGameThread())

		InAirState.bJumped    = !bPendingUpdate && (InAirState.bJumped || InAirState.bJumpRequested);
	InAirState.bJumpRequested = false;
}