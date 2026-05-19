#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "DeltaTimeSmootherSubsystem.generated.h"

class IConsoleVariable;

#ifndef DTS_DEBUG_VIEW_IN_SHIPPING
	#define DTS_DEBUG_VIEW_IN_SHIPPING 0
#endif
#define DTS_DEBUG_VIEW (!UE_BUILD_SHIPPING || DTS_DEBUG_VIEW_IN_SHIPPING)

/**
 * GameInstance subsystem for applying DeltaTime smoothing.
 *
 * Lives for the lifetime of a UGameInstance, so it's only active
 * during PIE and standalone game/server runs.
 * 
 * Not made for single-process multi-client scenarios where multiple
 * game instances can run simulteaneously, as it registers global hooks
 * and CVars. Consider disabling the plugin in those cases.
 */
UCLASS()
class UDeltaTimeSmootherSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

private:
	void Update();

	double SmoothDeltaTime(double RawDeltaTime);
	void MitigateHitchesDeltaSmoothing();
	void AbsorbHitchesDeltaSmoothing();

	void OnCVarChanged(IConsoleVariable* Var);
	void RefreshEnabledState();
	void SetDtsEnabled(bool bEnabled);
	bool IsRunningInPIE() const;

	FDelegateHandle SamplingInputHandle;
	FDelegateHandle CVarEnabledHandle;

	IConsoleVariable* CVarVSync;
	FDelegateHandle CVarVSyncHandle;

	IConsoleVariable* CVarRHISyncInterval;
	FDelegateHandle CVarRHISyncIntervalHandle;

#if WITH_EDITOR
	FDelegateHandle CVarAllowedInPIEHandle;

	IConsoleVariable* CVarVSyncInEditor;
	FDelegateHandle CVarVSyncInEditorHandle;
#endif

	// FIFO buffer. Index 0 = oldest (next to output), index SmoothBufferSize-1 = newest (just inserted).
	static constexpr int32 SmoothBufferSize = 10;
	double SmoothBuffer[SmoothBufferSize] = {};

	// How many frames of hitch mitigation are left. Otherwise default to hitch absorption.
	int32 HitchMitigationFrames = 0;

#if DTS_DEBUG_VIEW
	void DebugView(double RawDeltaTime, double SmoothedDeltaTime);
	double CurrentDebugGraphHeight = 0;
#endif
};
