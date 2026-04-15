#include "DeltaTimeSmoother.h"

#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "HAL/IConsoleManager.h"
#include "Math/UnrealMathUtility.h"
#include "Modules/ModuleManager.h"
#include "RHIUtilities.h"

#include "SlateIM.h"
#include "UnrealEngine.h"
#include "Engine/World.h"
#include "Engine/GameViewportClient.h"  

static TAutoConsoleVariable<int32> CVarEnabled(
	TEXT("DTS.Enabled"),
	1,
	TEXT("Enable (1, default) or disable (0) DeltaTimeSmoother."),
	ECVF_Default
);

static TAutoConsoleVariable<float> CVarMitigationThreshold(
	TEXT("DTS.MitigationThreshold"),
	0.07f,
	TEXT("The maximum delta time in seconds before hitch mitigation kicks in. 0 to disable. (Default: 0.07)"),
	ECVF_Default
);

static TAutoConsoleVariable<float> CVarPassthroughThreshold(
	TEXT("DTS.PassthroughThreshold"),
	0.1f,
	TEXT("No smoothing or mitigation will be performed on hitches over this delta time threshold. 0 to disable. (Default: 0.1)"),
	ECVF_Default
);

static TAutoConsoleVariable<int32> CVarDebugView(
	TEXT("DTS.DebugView"),
	0,
	TEXT("Enable (1) or disable (0, default) the debug view."),
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

	// FModuleManager::LoadModuleChecked<FSlateIMModule>("SlateIMModule");
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
	double OutputDeltaTime = RawDeltaTime;
	
	// Bigger hitches pass through. They can't be smoothed without speeding up too many frames.
	const double PassthroughThreshold = CVarPassthroughThreshold.GetValueOnGameThread();
	if (PassthroughThreshold > 0 && RawDeltaTime >= PassthroughThreshold)
	{
		// Average the other timings in the buffer.
		MitigateHitchesDeltaSmoothing();
		// Stop mitigating if it was active.
		HitchMitigationFrames = 0;
	}
	else
	{
		OutputDeltaTime = SmoothDeltaTime(RawDeltaTime);
	}

	if (CVarDebugView.GetValueOnGameThread())
	{
		DebugView(RawDeltaTime, OutputDeltaTime);
	}

	// We adjust deltaTime, but not FApp::CurrentTime. This is on purpose as we don't
	// want to mess up the App's "real time" for systems that depend on it. But this means that
	// deltaTime diverges a little bit during smoothing. Take that into account for any systems using both.
	FApp::SetDeltaTime(OutputDeltaTime);
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

	const double MitigationThreshold = CVarMitigationThreshold.GetValueOnGameThread();
	if (MitigationThreshold > 0 && RawDeltaTime >= MitigationThreshold)
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


void FDeltaTimeSmootherModule::DebugView(double RawDeltaTime, double SmoothedDeltaTime)
{
#if !UE_BUILD_SHIPPING
	if (GEngine && GEngine->GameViewport)
	{
		if (SlateIM::BeginViewportRoot("DTSDebugView", GEngine->GameViewport))
		{
			SlateIM::HAlign(HAlign_Fill);

			SlateIM::MinWidth(250.f);
			SlateIM::MaxWidth(250.f);
			SlateIM::BeginVerticalStack();

			SlateIM::HAlign(HAlign_Fill);
			SlateIM::Text(TEXT("DeltaTime Smoother"), FColor::Magenta);

			SlateIM::HAlign(HAlign_Fill);

			const double PassthroughThreshold = CVarPassthroughThreshold.GetValueOnGameThread();
			if (PassthroughThreshold > 0 && RawDeltaTime >= PassthroughThreshold)
			{
				SlateIM::Text(TEXT("Mode: Hitch Passthrough"), FColor::Red);
			}
			else if (HitchMitigationFrames > 0)
			{
				SlateIM::Text(FString::Printf(TEXT("Mode: Hitch Mitigation - frames left: %i"), HitchMitigationFrames), FColor::Orange);
			}
			else
			{
				SlateIM::Text(TEXT("Mode: Hitch Absorption"), FColor::Blue);
			}

			SlateIM::HAlign(HAlign_Fill);
			SlateIM::Text(FString::Printf(TEXT("Raw Delta Time: %f"), RawDeltaTime), FColor::White);
			SlateIM::HAlign(HAlign_Fill);
			SlateIM::Text(FString::Printf(TEXT("Smoothed Delta Time: %f"), SmoothedDeltaTime), FColor::White);
			SlateIM::MinHeight(100.f);
			SlateIM::MaxHeight(100.f);
			SlateIM::BeginGraph();

			// Hitch Passthrough line
			if (PassthroughThreshold > 0 && RawDeltaTime >= PassthroughThreshold)
			{
				double GraphHeight = FMath::Max(RawDeltaTime, 0.04);
				CurrentDebugGraphHeight = FMath::Max(GraphHeight, CurrentDebugGraphHeight);

				TArray<FVector2D> PassthroughLine{ {0, RawDeltaTime}, {1, RawDeltaTime} };
				SlateIM::GraphLine(PassthroughLine, { .XViewRange = FDoubleRange(0, 1), .YViewRange = FDoubleRange(0, CurrentDebugGraphHeight), .LineColor = FColor::Orange });
			}
			else
			{
				// Smoothing values
				TArray<FVector2D> GraphPoints;
				GraphPoints.Reserve(SmoothBufferSize * 2);
				double TotalWidth = 100;
				double MaxValue = 0;
				double LineWidth = TotalWidth / SmoothBufferSize;
				GraphPoints.Push({ 0, 0 });

				for (int32 i = 0; i < SmoothBufferSize; i++)
				{
					double CurrentX = LineWidth * i;
					double Value = SmoothBuffer[i];
					GraphPoints.Push({ CurrentX, Value });
					GraphPoints.Push({ CurrentX + LineWidth * 0.9, Value });
					GraphPoints.Push({ CurrentX + LineWidth * 0.9, 0 });
					GraphPoints.Push({ CurrentX + LineWidth, 0 });

					if (Value > MaxValue)
					{
						MaxValue = Value;
					}
				}

				double MaxHeight = FMath::Max(MaxValue, 0.04);
				CurrentDebugGraphHeight = FMath::Max(MaxValue, CurrentDebugGraphHeight);

				SlateIM::GraphLine(GraphPoints, { .XViewRange = FDoubleRange(-3, 100), .YViewRange = FDoubleRange(0, CurrentDebugGraphHeight), .LineColor = FColor::Orange });

				if (MaxHeight < CurrentDebugGraphHeight)
				{
					CurrentDebugGraphHeight = FMath::Lerp(CurrentDebugGraphHeight, MaxHeight, 0.1);
				}
			}

			// 16.66 ms line guide
			TArray<FVector2D> Guide60fps{ {0, 0.016666}, {1, 0.016666} };
			SlateIM::GraphLine(Guide60fps, { .XViewRange = FDoubleRange(0, 1), .YViewRange = FDoubleRange(0, CurrentDebugGraphHeight), .LineColor = FColor::Green });
			// 33.33 ms line guide
			TArray<FVector2D> Guide30fps{ {0, 0.033333}, {1, 0.033333} };
			SlateIM::GraphLine(Guide30fps, { .XViewRange = FDoubleRange(0, 1), .YViewRange = FDoubleRange(0, CurrentDebugGraphHeight), .LineColor = FColor::Red });

			SlateIM::EndGraph();
			SlateIM::EndVerticalStack();
		}
		SlateIM::EndRoot();
	}
#endif
}

IMPLEMENT_MODULE(FDeltaTimeSmootherModule, DeltaTimeSmoother)
