#include "DeltaTimeSmoother.h"

#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "HAL/IConsoleManager.h"
#include "Math/UnrealMathUtility.h"
#include "Modules/ModuleManager.h"

static TAutoConsoleVariable<int32> CVarEnabled(
	TEXT("DTS.Enabled"),
	1,
	TEXT("Enable (1) or disable (0) DeltaTimeSmoother."),
	ECVF_Default
);

void FDeltaTimeSmootherModule::StartupModule()
{
	// OnSamplingInput fires after UpdateTimeAndHandleMaxTickRate() sets FApp::DeltaTime
	// but before GEngine->Tick() reads it.
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
	FApp::SetDeltaTime(Output);
}

double FDeltaTimeSmootherModule::SmoothDeltaTime(double RawDeltaTime)
{
	// Fill the buffer with a reasonable default on the first call.
	if (!bSmoothBufferInitialized)
	{
		for (int32 i = 0; i < SmoothBufferSize; i++)
		{
			SmoothBuffer[i] = 1.0 / 60.0;
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
	
	// Smoothing pass. Work backwards (from newer to older deltas), find all bigger values and average them with newer
	// shorter deltas. That way a hitch averaging never "travels upwards", only downwards.
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

			// If we found some newer shorter deltas, average them with the hitch to pay off its debt.
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
	

	return Output;
}

IMPLEMENT_MODULE(FDeltaTimeSmootherModule, DeltaTimeSmoother)
