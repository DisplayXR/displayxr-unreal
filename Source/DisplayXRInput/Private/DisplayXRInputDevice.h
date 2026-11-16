// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "IInputDevice.h"
#include "IHapticDevice.h"
#include "XRMotionControllerBase.h"
#include "GenericPlatform/GenericApplicationMessageHandler.h"
#include "DisplayXRControllerTypes.h"

/**
 * Controllers and styluses from any DisplayXR input provider, as Unreal input:
 *
 *  - IInputDevice: button / axis events for the FDisplayXRKeys keys, so they
 *    work in Enhanced Input mapping contexts and legacy bindings alike.
 *  - IMotionController ("DisplayXR"): grip / aim / tip poses for
 *    UMotionControllerComponent (FDisplayXRMotionSources), plus the custom
 *    parameters "TipForce" and "MiddleForce".
 *  - IHapticDevice: haptic effects and force feedback -> xrApplyHapticFeedback.
 *
 * State comes from FDisplayXRControllerAPI (the core module syncs the OpenXR
 * action set right after the view locate). Everything answers "nothing"
 * unless DisplayXR is the active XR system, so it is inert next to another
 * XR plug-in.
 */
class FDisplayXRInputDevice : public IInputDevice, public FXRMotionControllerBase, public IHapticDevice
{
public:
	explicit FDisplayXRInputDevice(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler);
	virtual ~FDisplayXRInputDevice();

	/** The IMotionController device type name ("DisplayXR"). */
	static FName DeviceTypeName();

	// --- IInputDevice ---
	virtual void Tick(float DeltaTime) override { CurrentDeltaTime = DeltaTime; }
	virtual void SendControllerEvents() override;
	virtual void SetMessageHandler(const TSharedRef<FGenericApplicationMessageHandler>& InMessageHandler) override { MessageHandler = InMessageHandler; }
	virtual bool Exec(UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar) override { return false; }
	virtual void SetChannelValue(int32 ControllerId, FForceFeedbackChannelType ChannelType, float Value) override;
	virtual void SetChannelValues(int32 ControllerId, const FForceFeedbackValues& Values) override;
	virtual bool SupportsForceFeedback(int32 ControllerId) override;
	virtual IHapticDevice* GetHapticDevice() override { return this; }

	// --- IMotionController ---
	virtual FName GetMotionControllerDeviceTypeName() const override { return DeviceTypeName(); }
	using FXRMotionControllerBase::GetControllerOrientationAndPosition;
	virtual bool GetControllerOrientationAndPosition(const int32 ControllerIndex, const FName MotionSource,
		FRotator& OutOrientation, FVector& OutPosition, float WorldToMetersScale) const override;
	virtual ETrackingStatus GetControllerTrackingStatus(const int32 ControllerIndex, const FName MotionSource) const override;
	virtual void EnumerateSources(TArray<FMotionControllerSource>& SourcesOut) const override;
	virtual float GetCustomParameterValue(const FName MotionSource, FName ParameterName, bool& bOutValueFound) const override;

	// --- IHapticDevice ---
	virtual void SetHapticFeedbackValues(int32 ControllerId, int32 Hand, const FHapticFeedbackValues& Values) override;
	virtual void GetHapticFrequencyRange(float& MinFrequency, float& MaxFrequency) const override;
	virtual float GetHapticAmplitudeScale() const override { return 1.0f; }

	/** Resolve a motion source to a hand (0 / 1) and a pose of that hand.
	 *  Returns nullptr for a source this device does not serve. */
	static const FDisplayXRControllerPose* ResolveSource(const FDisplayXRControllerSnapshot& Snap, FName MotionSource, int32& OutHand);

	/** True when DisplayXR is the engine's active XR system. */
	static bool IsDisplayXRActive();

private:
	struct FButtonState
	{
		bool bPressed[2][8] = {};
		float Axis[2][2] = {};
	};

	void SetHaptic(int32 HandIndex, float Amplitude, float FrequencyHz);

	TSharedRef<FGenericApplicationMessageHandler> MessageHandler;
	FButtonState Previous;
	float CurrentDeltaTime = 0.0f;
	bool bHapticActive[2] = {false, false};
};
