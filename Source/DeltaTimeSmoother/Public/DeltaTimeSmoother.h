#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

/**
 * DeltaTimeSmoother plugin.
 *
 * Smooths delta times, reducing jitter.
 */
class FDeltaTimeSmootherModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void OnSamplingInput();

	double SmoothDeltaTime(double RawDeltaTime);
	void MitigateHitchesDeltaSmoothing();
	void AbsorbHitchesDeltaSmoothing();

	FDelegateHandle SamplingInputHandle;

	// FIFO buffer. Index 0 = oldest (next to output), index SmoothBufferSize-1 = newest (just inserted).
	static constexpr int32 SmoothBufferSize = 10;
	double SmoothBuffer[SmoothBufferSize] = {};
	bool bSmoothBufferInitialized = false;

	// How many frames of hitch mitigation are left. Otherwise default to hitch absorption.
	int32 HitchMitigationFrames = 0;
};
