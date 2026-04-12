# DeltaTimeSmoother

Eliminates jitter (also known as animation error or micro-hitches) by smoothing `DeltaTime` spikes across multiple frames without introducing persistent time drift.

## Overview
As explained in "A Frame's Life" at Unreal Fest Chicago 2026 (link coming soon), jitter happens when you have uneven delta times that don't match the frame pacing of the frames displayed to the user.

The DeltaTimeSmoother plugin resolves this in two ways:

* Smaller hitches (configured as <100ms by default) can often be absorbed by the slack in the rendering pipeline. Subsequent frames usually run faster while the pipeline fills up again. The plugin takes advantage of that and will pay off the debt of a hitch by averaging it's delta time only with faster subsequent delta times.
* Larger hitches cannot be absorbed, and must instead be mitigated. When the plugin detects a larger hitch, it will switch to mitigation mode where it immediately averages out all the frames until the hitch passes through the FIFO buffer (sized 10 by default). The delta time will immediately increase, and the recovery will be smoothed down. After the hitch has been mitigated, the plugin switches back to hitch absorption mode.

Both of these methods conserve the total time accross all buffer entries, so no simulation time is ever lost or gained.

Note: `FApp::GetCurrentTime() - FApp::GetLastTime()` will not equal `FApp::GetDeltaTime()` exactly after smoothing, although they will always be somewhat close due to the total time conservation. Take that into account if you use both `FApp::GetCurrentTime()` and `FApp::GetDeltaTime()`.

## Installation

Put the plugin in either your `ProjectName/Plugins` or `Engine/Plugins` directory.

To customize it, override the CVars in the `[ConsoleVariables]` section of your `DefaultEngine.ini` file.

## Console Variables

| CVar | Default | Description |
|---|---|---|
| `DTS.Enabled` | `1` | Enable (1) or disable (0) the smoother entirely. |
| `DTS.MitigationThreshold` | `0.1` | Raw delta time in seconds that triggers hitch mitigation mode. |

You can set the CVars in the `[ConsoleVariables]` section of your `DefaultEngine.ini` file.

## Technical Details

### Normal Mode: Hitch Absorption (`AbsorbHitchesDeltaSmoothing()`)

Active when no large hitch has been recently detected. Works backwards through the buffer from newest to oldest: when an older entry is larger than its newer neighbor, the older hitch is averaged with contiguous subsequent shorter entries. This distributes the hitch's "debt" forward in time: newer faster frames pay for older slower ones. The backwards iteration means newer hitches are resolved first, and older hitches operate on already-averaged values, driving the buffer toward a consistent flat output.

### Hitch Mitigation Mode (`MitigateHitchesDeltaSmoothing()`)

Triggered when a raw delta exceeds `DTS.MitigationThreshold` (default: 0.1 seconds). Immediately averages the entire buffer (including the triggering frame) to promptly react to the hitch without waiting for it to propagate through the FIFO. Runs for `SmoothBufferSize` frames, incorporating each new incoming raw value into the running average. After `SmoothBufferSize` frames, the entire buffer has turned over with post-hitch data and normal absorption resumes.

### Hook Point

The smoother binds to `FCoreDelegates::OnSamplingInput`, which fires after `UpdateTimeAndHandleMaxTickRate()` sets `FApp::DeltaTime` but before `GEngine->Tick()` propagates it to `UWorld::Tick`. The smoothed value is written back via `FApp::SetDeltaTime()`.

This is more convenient than using a `UEngineCustomTimeStep`, as a custom time step completely skips all the engine's built-in functionality in `UpdateTimeAndHandleMaxTickRate`. This plugin is supposed to complement most of that functionality, not replace it.

A better hook point might be via `ILatencyMarkerModule::SetSimulationLatencyMarkerStart`, as it is called immediately after `UpdateTimeAndHandleMaxTickRate()`, but I haven't looked into if that's needed yet.

## MaxDeltaTime

Unreal runs uncapped delta times by default, so very large hitches (>300ms) will have extreme recovery. By default, UE will simulate the next frame with an uncapped delta time matching the hitch in size, which is jarring. With DeltaTimeSmoother, the delta time won't have this large of a skip, but the game will run comically fast for a short time while the hitch is smoothed down.

I recommend setting `MaxDeltaTime` to something like 0.3 so that very large hitches don't have such extreme side effects. You can do that by putting this into your **DefaultEngine.ini**:

    [/Script/Engine.GameEngine]
    MaxDeltaTime=0.3
