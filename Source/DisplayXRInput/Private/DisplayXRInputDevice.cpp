// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#include "DisplayXRInputDevice.h"
#include "DisplayXRInputKeys.h"
#include "Engine/Engine.h"
#include "IXRTrackingSystem.h"
#include "GenericPlatform/IInputInterface.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "Features/IModularFeatures.h"

DEFINE_LOG_CATEGORY_STATIC(LogDisplayXRInput, Log, All);

// =============================================================================
// Keys and motion sources
// =============================================================================

const FName FDisplayXRKeys::MenuCategory(TEXT("DisplayXR"));

const FKey FDisplayXRKeys::Left_Select(TEXT("DisplayXR_Left_Select"));
const FKey FDisplayXRKeys::Left_Menu(TEXT("DisplayXR_Left_Menu"));
const FKey FDisplayXRKeys::Left_System(TEXT("DisplayXR_Left_System"));
const FKey FDisplayXRKeys::Left_Tip_Force(TEXT("DisplayXR_Left_Tip_Force"));
const FKey FDisplayXRKeys::Left_Middle_Force(TEXT("DisplayXR_Left_Middle_Force"));
const FKey FDisplayXRKeys::Left_Front_Click(TEXT("DisplayXR_Left_Front_Click"));
const FKey FDisplayXRKeys::Left_Front_DoubleTap(TEXT("DisplayXR_Left_Front_DoubleTap"));
const FKey FDisplayXRKeys::Left_Back_Click(TEXT("DisplayXR_Left_Back_Click"));
const FKey FDisplayXRKeys::Left_Back_DoubleTap(TEXT("DisplayXR_Left_Back_DoubleTap"));
const FKey FDisplayXRKeys::Left_Docked(TEXT("DisplayXR_Left_Docked"));

const FKey FDisplayXRKeys::Right_Select(TEXT("DisplayXR_Right_Select"));
const FKey FDisplayXRKeys::Right_Menu(TEXT("DisplayXR_Right_Menu"));
const FKey FDisplayXRKeys::Right_System(TEXT("DisplayXR_Right_System"));
const FKey FDisplayXRKeys::Right_Tip_Force(TEXT("DisplayXR_Right_Tip_Force"));
const FKey FDisplayXRKeys::Right_Middle_Force(TEXT("DisplayXR_Right_Middle_Force"));
const FKey FDisplayXRKeys::Right_Front_Click(TEXT("DisplayXR_Right_Front_Click"));
const FKey FDisplayXRKeys::Right_Front_DoubleTap(TEXT("DisplayXR_Right_Front_DoubleTap"));
const FKey FDisplayXRKeys::Right_Back_Click(TEXT("DisplayXR_Right_Back_Click"));
const FKey FDisplayXRKeys::Right_Back_DoubleTap(TEXT("DisplayXR_Right_Back_DoubleTap"));
const FKey FDisplayXRKeys::Right_Docked(TEXT("DisplayXR_Right_Docked"));

const FName FDisplayXRMotionSources::Left(TEXT("Left"));
const FName FDisplayXRMotionSources::Right(TEXT("Right"));
const FName FDisplayXRMotionSources::LeftGrip(TEXT("LeftGrip"));
const FName FDisplayXRMotionSources::RightGrip(TEXT("RightGrip"));
const FName FDisplayXRMotionSources::LeftAim(TEXT("LeftAim"));
const FName FDisplayXRMotionSources::RightAim(TEXT("RightAim"));
const FName FDisplayXRMotionSources::LeftTip(TEXT("LeftTip"));
const FName FDisplayXRMotionSources::RightTip(TEXT("RightTip"));

namespace
{
	// Button slots in FButtonState::bPressed, in this order.
	enum EDxrButton : int32
	{
		DxrButton_Select = 0,
		DxrButton_Menu,
		DxrButton_System,
		DxrButton_FrontClick,
		DxrButton_FrontDoubleTap,
		DxrButton_BackClick,
		DxrButton_BackDoubleTap,
		DxrButton_Docked,
		DxrButton_Count
	};

	const FKey& DxrButtonKey(int32 Hand, int32 Button)
	{
		static const FKey* const Keys[2][DxrButton_Count] = {
			{&FDisplayXRKeys::Left_Select, &FDisplayXRKeys::Left_Menu, &FDisplayXRKeys::Left_System,
			 &FDisplayXRKeys::Left_Front_Click, &FDisplayXRKeys::Left_Front_DoubleTap,
			 &FDisplayXRKeys::Left_Back_Click, &FDisplayXRKeys::Left_Back_DoubleTap, &FDisplayXRKeys::Left_Docked},
			{&FDisplayXRKeys::Right_Select, &FDisplayXRKeys::Right_Menu, &FDisplayXRKeys::Right_System,
			 &FDisplayXRKeys::Right_Front_Click, &FDisplayXRKeys::Right_Front_DoubleTap,
			 &FDisplayXRKeys::Right_Back_Click, &FDisplayXRKeys::Right_Back_DoubleTap, &FDisplayXRKeys::Right_Docked},
		};
		return *Keys[Hand][Button];
	}

	bool DxrButtonValue(const FDisplayXRControllerState& S, int32 Button)
	{
		switch (Button)
		{
		case DxrButton_Select:         return S.bSelect;
		case DxrButton_Menu:           return S.bMenu;
		case DxrButton_System:         return S.bSystemClick;
		case DxrButton_FrontClick:     return S.bFrontClick;
		case DxrButton_FrontDoubleTap: return S.bFrontDoubleTap;
		case DxrButton_BackClick:      return S.bBackClick;
		case DxrButton_BackDoubleTap:  return S.bBackDoubleTap;
		case DxrButton_Docked:         return S.bDocked;
		default:                       return false;
		}
	}

	const FKey& DxrAxisKey(int32 Hand, int32 Axis)
	{
		static const FKey* const Keys[2][2] = {
			{&FDisplayXRKeys::Left_Tip_Force, &FDisplayXRKeys::Left_Middle_Force},
			{&FDisplayXRKeys::Right_Tip_Force, &FDisplayXRKeys::Right_Middle_Force},
		};
		return *Keys[Hand][Axis];
	}
}

// =============================================================================
// FDisplayXRInputDevice
// =============================================================================

FDisplayXRInputDevice::FDisplayXRInputDevice(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler)
	: MessageHandler(InMessageHandler)
{
	static_assert(DxrButton_Count == 8, "FButtonState::bPressed holds 8 buttons per hand");
	IModularFeatures::Get().RegisterModularFeature(GetModularFeatureName(), static_cast<IMotionController*>(this));
	UE_LOG(LogDisplayXRInput, Log, TEXT("DisplayXR: input device created (IMotionController '%s')"), *DeviceTypeName().ToString());
}

FDisplayXRInputDevice::~FDisplayXRInputDevice()
{
	IModularFeatures::Get().UnregisterModularFeature(GetModularFeatureName(), static_cast<IMotionController*>(this));
}

FName FDisplayXRInputDevice::DeviceTypeName()
{
	static const FName Name(TEXT("DisplayXR"));
	return Name;
}

bool FDisplayXRInputDevice::IsDisplayXRActive()
{
	return GEngine && GEngine->XRSystem.IsValid() && GEngine->XRSystem->GetSystemName() == DeviceTypeName();
}

void FDisplayXRInputDevice::SendControllerEvents()
{
	FDisplayXRControllerSnapshot Snap;
	if (!IsDisplayXRActive() || !FDisplayXRControllerAPI::GetSnapshot(Snap))
	{
		// Release anything held when the session goes away.
		Snap = FDisplayXRControllerSnapshot();
	}

	IPlatformInputDeviceMapper& DeviceMapper = IPlatformInputDeviceMapper::Get();
	const FPlatformUserId User = DeviceMapper.GetPrimaryPlatformUser();
	const FInputDeviceId Device = DeviceMapper.GetDefaultInputDevice();

	for (int32 Hand = 0; Hand < 2; ++Hand)
	{
		const FDisplayXRControllerState& S = Snap.Hands[Hand];
		for (int32 B = 0; B < DxrButton_Count; ++B)
		{
			const bool bNow = S.bActive && DxrButtonValue(S, B);
			bool& bWas = Previous.bPressed[Hand][B];
			if (bNow != bWas)
			{
				const FName Key = DxrButtonKey(Hand, B).GetFName();
				if (bNow)
				{
					MessageHandler->OnControllerButtonPressed(Key, User, Device, /*IsRepeat*/ false);
				}
				else
				{
					MessageHandler->OnControllerButtonReleased(Key, User, Device, /*IsRepeat*/ false);
				}
				bWas = bNow;
			}
		}

		const float Axes[2] = {S.bActive ? S.TipForce : 0.0f, S.bActive ? S.MiddleForce : 0.0f};
		for (int32 A = 0; A < 2; ++A)
		{
			float& Was = Previous.Axis[Hand][A];
			// Every frame while non-zero (Enhanced Input samples axes per frame),
			// plus the one zero that ends a press.
			if (Axes[A] != 0.0f || Was != 0.0f)
			{
				MessageHandler->OnControllerAnalog(DxrAxisKey(Hand, A).GetFName(), User, Device, Axes[A]);
			}
			Was = Axes[A];
		}
	}
}

const FDisplayXRControllerPose* FDisplayXRInputDevice::ResolveSource(const FDisplayXRControllerSnapshot& Snap, FName MotionSource, int32& OutHand)
{
	enum class EPose : uint8 { Grip, Aim, Tip };
	EPose Pose;
	if (MotionSource == FDisplayXRMotionSources::Left || MotionSource == FDisplayXRMotionSources::LeftGrip)
	{
		OutHand = 0; Pose = EPose::Grip;
	}
	else if (MotionSource == FDisplayXRMotionSources::Right || MotionSource == FDisplayXRMotionSources::RightGrip)
	{
		OutHand = 1; Pose = EPose::Grip;
	}
	else if (MotionSource == FDisplayXRMotionSources::LeftAim)
	{
		OutHand = 0; Pose = EPose::Aim;
	}
	else if (MotionSource == FDisplayXRMotionSources::RightAim)
	{
		OutHand = 1; Pose = EPose::Aim;
	}
	else if (MotionSource == FDisplayXRMotionSources::LeftTip)
	{
		OutHand = 0; Pose = EPose::Tip;
	}
	else if (MotionSource == FDisplayXRMotionSources::RightTip)
	{
		OutHand = 1; Pose = EPose::Tip;
	}
	else
	{
		OutHand = -1;
		return nullptr;
	}

	const FDisplayXRControllerState& S = Snap.Hands[OutHand];
	switch (Pose)
	{
	case EPose::Grip: return &S.Grip;
	case EPose::Aim:  return &S.Aim;
	default:
		// A profile without a tip (a generic controller): the aim pose is
		// where it points from, the closest stand-in.
		return S.Tip.bValid ? &S.Tip : &S.Aim;
	}
}

bool FDisplayXRInputDevice::GetControllerOrientationAndPosition(const int32 ControllerIndex, const FName MotionSource,
	FRotator& OutOrientation, FVector& OutPosition, float WorldToMetersScale) const
{
	if (ControllerIndex != 0 || !IsDisplayXRActive())
	{
		return false;
	}
	FDisplayXRControllerSnapshot Snap;
	if (!FDisplayXRControllerAPI::GetSnapshot(Snap))
	{
		return false;
	}
	int32 Hand = -1;
	const FDisplayXRControllerPose* Pose = ResolveSource(Snap, MotionSource, Hand);
	if (!Pose || !Pose->bValid)
	{
		return false;
	}
	OutOrientation = Pose->Orientation.Rotator();
	OutPosition = Pose->PositionMeters * WorldToMetersScale;
	return true;
}

ETrackingStatus FDisplayXRInputDevice::GetControllerTrackingStatus(const int32 ControllerIndex, const FName MotionSource) const
{
	if (ControllerIndex != 0 || !IsDisplayXRActive())
	{
		return ETrackingStatus::NotTracked;
	}
	FDisplayXRControllerSnapshot Snap;
	if (!FDisplayXRControllerAPI::GetSnapshot(Snap))
	{
		return ETrackingStatus::NotTracked;
	}
	int32 Hand = -1;
	const FDisplayXRControllerPose* Pose = ResolveSource(Snap, MotionSource, Hand);
	return (Pose && Pose->bValid && Pose->bTracked) ? ETrackingStatus::Tracked : ETrackingStatus::NotTracked;
}

void FDisplayXRInputDevice::EnumerateSources(TArray<FMotionControllerSource>& SourcesOut) const
{
	for (const FName& Name : {FDisplayXRMotionSources::Left, FDisplayXRMotionSources::Right,
			 FDisplayXRMotionSources::LeftGrip, FDisplayXRMotionSources::RightGrip,
			 FDisplayXRMotionSources::LeftAim, FDisplayXRMotionSources::RightAim,
			 FDisplayXRMotionSources::LeftTip, FDisplayXRMotionSources::RightTip})
	{
		FMotionControllerSource Source(Name);
#if WITH_EDITOR
		Source.EditorCategory = FName(TEXT("DisplayXR"));
#endif
		SourcesOut.Add(Source);
	}
}

float FDisplayXRInputDevice::GetCustomParameterValue(const FName MotionSource, FName ParameterName, bool& bOutValueFound) const
{
	bOutValueFound = false;
	FDisplayXRControllerSnapshot Snap;
	if (!IsDisplayXRActive() || !FDisplayXRControllerAPI::GetSnapshot(Snap))
	{
		return 0.0f;
	}
	int32 Hand = -1;
	if (!ResolveSource(Snap, MotionSource, Hand) || Hand < 0)
	{
		return 0.0f;
	}
	static const FName TipForceName(TEXT("TipForce"));
	static const FName MiddleForceName(TEXT("MiddleForce"));
	if (ParameterName == TipForceName)
	{
		bOutValueFound = true;
		return Snap.Hands[Hand].TipForce;
	}
	if (ParameterName == MiddleForceName)
	{
		bOutValueFound = true;
		return Snap.Hands[Hand].MiddleForce;
	}
	return 0.0f;
}

void FDisplayXRInputDevice::SetHaptic(int32 HandIndex, float Amplitude, float FrequencyHz)
{
	if (HandIndex < 0 || HandIndex > 1)
	{
		return;
	}
	if (Amplitude <= 0.0f)
	{
		if (bHapticActive[HandIndex])
		{
			FDisplayXRControllerAPI::StopHaptic(HandIndex);
		}
		bHapticActive[HandIndex] = false;
		return;
	}
	// Unreal re-sends haptic values every frame while an effect plays, so one
	// frame's worth of vibration per call (the runtime's minimum before the
	// first tick), like Epic's OpenXR input.
	bHapticActive[HandIndex] =
		FDisplayXRControllerAPI::ApplyHaptic(HandIndex, Amplitude, FrequencyHz, CurrentDeltaTime) || bHapticActive[HandIndex];
}

void FDisplayXRInputDevice::SetHapticFeedbackValues(int32 ControllerId, int32 Hand, const FHapticFeedbackValues& Values)
{
	if (ControllerId != 0 || !IsDisplayXRActive())
	{
		return;
	}
	// GetHapticFrequencyRange reports 0..0, so Unreal's normalised frequency
	// arrives as 0 = let the runtime pick.
	const float Amplitude = Values.Amplitude;
	if (Hand == (int32)EControllerHand::Left || Hand == (int32)EControllerHand::AnyHand)
	{
		SetHaptic(0, Amplitude, 0.0f);
	}
	if (Hand == (int32)EControllerHand::Right || Hand == (int32)EControllerHand::AnyHand)
	{
		SetHaptic(1, Amplitude, 0.0f);
	}
}

void FDisplayXRInputDevice::GetHapticFrequencyRange(float& MinFrequency, float& MaxFrequency) const
{
	// XR_FREQUENCY_UNSPECIFIED: the runtime chooses.
	MinFrequency = 0.0f;
	MaxFrequency = 0.0f;
}

void FDisplayXRInputDevice::SetChannelValue(int32 ControllerId, FForceFeedbackChannelType ChannelType, float Value)
{
	if (ControllerId != 0 || !IsDisplayXRActive())
	{
		return;
	}
	const bool bLeft = ChannelType == FForceFeedbackChannelType::LEFT_LARGE || ChannelType == FForceFeedbackChannelType::LEFT_SMALL;
	SetHaptic(bLeft ? 0 : 1, Value, 0.0f);
}

void FDisplayXRInputDevice::SetChannelValues(int32 ControllerId, const FForceFeedbackValues& Values)
{
	if (ControllerId != 0 || !IsDisplayXRActive())
	{
		return;
	}
	SetHaptic(0, FMath::Max(Values.LeftLarge, Values.LeftSmall), 0.0f);
	SetHaptic(1, FMath::Max(Values.RightLarge, Values.RightSmall), 0.0f);
}

bool FDisplayXRInputDevice::SupportsForceFeedback(int32 ControllerId)
{
	return ControllerId == 0 && FDisplayXRControllerAPI::IsAvailable();
}
