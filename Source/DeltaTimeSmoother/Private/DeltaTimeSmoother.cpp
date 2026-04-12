#include "DeltaTimeSmoother.h"

#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "HAL/IConsoleManager.h"
#include "Math/UnrealMathUtility.h"
#include "Modules/ModuleManager.h"
#include "RHIUtilities.h"

static TAutoConsoleVariable<int32> CVarEnabled(
	TEXT("DTS.Enabled"),
	1,
	TEXT("Enable (1, default) or disable (0) DeltaTimeSmoother."),
	ECVF_Default
);

static TAutoConsoleVariable<float> CVarMitigationThreshold(
	TEXT("DTS.MitigationThreshold"),
	0.1f,
	TEXT("The maximum delta time in seconds before hitch mitigation kicks in. (Default: 0.1)"),
	ECVF_Default
);

void FDeltaTimeSmootherModule::StartupModule()
{
	// We hook into OnSamplingInput as it fires after UpdateTimeAndHandleMaxTickRate() sets FApp::DeltaTime
	// but before GEngine->Tick() reads it.
	// This is much easier than using a UEngineCustomTimeStep, as a custom time step completely skips all the engine's
	// built-in functionality in UpdateTimeAndHandleMaxTickRate. This plugin is supposed to complement most of that
	// functionality instead of replacing it.
	// An even better hook might be ILatencyMarkerModule::SetSimulationLatencyMarkerStart, as it is called
	// immediately after UpdateTimeAndHandleMaxTickRate().
	SamplingInputHandle = FCoreDelegates::OnSamplingInput.AddRaw(this, &FDeltaTimeSmootherModule::OnSamplingInput);
}

void FDeltaTimeSmootherModule::ShutdownModule()
{
	FCoreDelegates::OnSamplingInput.Remove(SamplingInputHandle);
	SamplingInputHandle.Reset();
}

void FDeltaTimeSmootherModule::OnSamplingInput()
{
	// This fires every frame

	if (!CVarEnabled.GetValueOnGameThread())
	{
		return;
	}

	const double RawDeltaTime = FApp::GetDeltaTime();
	const double Output = SmoothDeltaTime(RawDeltaTime);
	// We adjust deltaTime, but not FApp::CurrentTime. This is on purpose as we don't
	// want to mess up the App's "real time" for systems that depend on it. But this means that
	// deltaTime diverges a little bit during smoothing. Take that into account for any systems using both.
	FApp::SetDeltaTime(Output);
}


double FDeltaTimeSmootherModule::SmoothDeltaTime(double RawDeltaTime)
{
	// Fill the buffer with a reasonable default on the first call.
	if (!bSmoothBufferInitialized)
	{
		double TargetFrameRate = (double)FPlatformMisc::GetMaxRefreshRate() / FMath::Max(1u, RHIGetSyncInterval());
		double TargetDeltaTime = 1.f / TargetFrameRate;
		for (int32 i = 0; i < SmoothBufferSize; i++)
		{
			SmoothBuffer[i] = TargetDeltaTime;
		}
		bSmoothBufferInitialized = true;
	}

	const double Output = SmoothBuffer[0];
	// Shift the buffer left: the oldest value falls off, a slot opens at the newest end.
	for (int32 i = 0; i < SmoothBufferSize - 1; i++)
	{
		SmoothBuffer[i] = SmoothBuffer[i + 1];
	}
	SmoothBuffer[SmoothBufferSize - 1] = RawDeltaTime;


	if (RawDeltaTime >= CVarMitigationThreshold.GetValueOnGameThread())
	{
		// A hitch which we can't absorb was encountered.
		// Switch to hitch mitigation mode.
		HitchMitigationFrames = SmoothBufferSize;
	}

	if (HitchMitigationFrames > 0)
	{
		MitigateHitchesDeltaSmoothing();
		HitchMitigationFrames--;
	}
	else
	{
		AbsorbHitchesDeltaSmoothing();
	}

	return Output;
}

void FDeltaTimeSmootherModule::MitigateHitchesDeltaSmoothing()
{
	// A hitch was encountered, average all the delta times in the buffer
	// to react to it immediately. This prevents a SmoothBufferSized "lag" before
	// reacting to a hitch.
	double TotalSum = 0;
	for (int32 i = 0; i < SmoothBufferSize; i++)
	{
		TotalSum += SmoothBuffer[i];
	}
	double AveragedDeltaTime = TotalSum / SmoothBufferSize;
	for (int32 i = 0; i < SmoothBufferSize; i++)
	{
		SmoothBuffer[i] = AveragedDeltaTime;
	}
}

void FDeltaTimeSmootherModule::AbsorbHitchesDeltaSmoothing()
{
	// Default smoothing. Minor hitches will be absorbed by subsequent faster frames.

	// Work backwards (from newer to older deltas), find all bigger values and average them with newer
	// shorter deltas. That way hitch averaging never "travels upwards", only downwards.
	// Basically, newer shorter deltas pay the debt of older hitches.
	for (int32 i = SmoothBufferSize - 2; i >= 0; i--)
	{
		double A = SmoothBuffer[i]; // The older deltatime
		double B = SmoothBuffer[i + 1]; // The newer deltatime

		// If A is bigger, average it out with all newer shorter deltas.
		if (A > B)
		{
			double CurrentSum = A;
			int32 CurrentCount = 1;

			// Seek forward while newer delta times are shorter or equal. We'll use these to pay off the debt of the bigger one.
			for (int32 j = i + 1; j < SmoothBufferSize; j++)
			{
				double C = SmoothBuffer[j];
				if (C <= B)
				{
					CurrentSum += C;
					CurrentCount++;
				}
				else
				{
					// Found a bigger one, stop here. We don't want hitches to touch older shorter deltas.
					// Only average hitches with newer shorter deltas.
					break;
				}
			}

			// If we found newer shorter deltas, average them with the hitch to pay off its debt.
			if (CurrentCount > 1)
			{
				int32 FromIndex = i;
				int32 ToIndex = FromIndex + CurrentCount;
				double NewAverage = CurrentSum / CurrentCount;
				for (int32 j = FromIndex; j < ToIndex; j++)
				{
					SmoothBuffer[j] = NewAverage;
				}
			}
		}
	}
}

IMPLEMENT_MODULE(FDeltaTimeSmootherModule, DeltaTimeSmoother)
