# DeltaTimeSmoother

Unreal Engine plugin that eliminates jitter (also known as animation error or micro-hitches) by smoothing small  `DeltaTime` spikes across multiple frames without introducing persistent time drift.

DeltaTimeSmoother works in situations where the game is able to maintain its target framerate, but has some micro-hitches that can be absorbed by queued up work in the rendering pipeline.

* DeltaTimeSmoother is **not** designed to smooth out larger hitches. You wouldn't want that, as it would speed up the simulation noticably when the delta time is catching up.
* DeltaTimeSmoother needs queued up work in the rendering pipeline.  
It does **not** benefit when:
    * Your game is CPU bound and over the vsync frame time budget.
    * Your game syncs the Game Thread to VSync (`r.GTSyncType=2`) .
    * Your game uses Game Thread throttling to starve the pipeline, like via NVIDIA Reflex, AMD Anti-Lag 2, or Intel Xe Low Latency.
    

## Overview
As explained in "A Frame's Life" at Unreal Fest Chicago 2026 (link coming soon), jitter happens when you have uneven delta times that don't match the frame pacing of the frames displayed to the user.

The DeltaTimeSmoother plugin resolves this in a few ways:

* Smaller hitches (configured as <70ms by default) can often be absorbed by queued up work in the rendering pipeline. Subsequent frames usually run faster while the pipeline fills up again. The plugin takes advantage of that and will pay off the debt of a hitch by averaging it's delta time only with faster subsequent delta times.
* Medium hitches cannot be absorbed, but we may be able to mitigate them without it being too noticable. When the plugin detects a medium hitch (configured as <100ms by default), it will switch to mitigation mode where it immediately averages out all the frames until the hitch passes through the FIFO buffer (sized 10 by default). The delta time will immediately increase, and the recovery will be smoothed down. After the hitch has been mitigated, the plugin switches back to hitch absorption mode.
* Larger hitches cannot be absorbed or mitigated. The plugin lets them through without touching them (configured as >=100ms by default) for the least jarring recovery. Optionally pair this with a `MaxDeltaTime` ceiling (see below) to not have DeltaTime simulate totally unbound.

The smoothing methods conserve the total time accross all buffer entries, so no simulation time is ever lost or gained.

Note: `FApp::GetCurrentTime() - FApp::GetLastTime()` will not equal `FApp::GetDeltaTime()` exactly after smoothing, although they will always be somewhat close due to the total time conservation. Take that into account if you use both `FApp::GetCurrentTime()` and `FApp::GetDeltaTime()`.

## Installation

Put the plugin in either your `ProjectName/Plugins` or `Engine/Plugins` directory.

To customize it, override the CVars in the `[ConsoleVariables]` section of your `DefaultEngine.ini` file.

## Console Variables

| CVar | Default | Description |
|---|---|---|
| `DTS.Enabled` | `1` | Enable (1) or disable (0) the smoother entirely. |
| `DTS.MitigationThreshold` | `0.07` | The maximum delta time in seconds before hitch mitigation kicks in. 0 to disable. |
| `DTS.PassthroughThreshold` | `0.1` | No smoothing or mitigation will be performed on hitches over this delta time threshold, they will instead pass through untouched. 0 to disable. |
| `DTS.DebugView` | `0` | Enable (1) or disable (0) the debug view. |

You can set the CVars in the `[ConsoleVariables]` section of your `DefaultEngine.ini` file.

## Technical Details

### Hitch Absorption Mode (`AbsorbHitchesDeltaSmoothing()`)

The normal mode. Works backwards through the buffer from newest to oldest: when an older entry is larger than its newer neighbor, the older hitch is averaged with contiguous subsequent shorter entries. This distributes the hitch's "debt" forward in time: newer faster frames pay for older slower ones. The backwards iteration means newer hitches are resolved first, and older hitches operate on already-averaged values, driving the buffer toward a consistent flat output.

### Hitch Mitigation Mode (`MitigateHitchesDeltaSmoothing()`)

Triggered when a raw delta exceeds `DTS.MitigationThreshold`. Immediately averages the entire buffer (including the triggering frame) to promptly react to the hitch without waiting for it to propagate through the FIFO. Runs for `SmoothBufferSize` frames, incorporating each new incoming raw value into the running average. After `SmoothBufferSize` frames, the entire buffer has turned over with post-hitch data and normal absorption resumes. This can smooth out some medium sized hitches without being too noticeable. Tweak the cvar according to your needs, or disable entirely if it doesn't fit your game.

### Hitch Passthrough Mode

Triggered when a raw delta exceeds `DTS.PassthroughThreshold`. Bigger hitches can't be smoothed out without noticeable simulation speed up, so it's better to just let the time skip forward in the next frame. Optionally, set `MaxDeltaTime` to put a ceiling on the amount of time that can be skipped, see more at the bottom of this document.

### Hook Point

The smoother binds to `FCoreDelegates::OnSamplingInput`, which fires after `UpdateTimeAndHandleMaxTickRate()` sets `FApp::DeltaTime` but before `GEngine->Tick()` propagates it to `UWorld::Tick`. The smoothed value is written back via `FApp::SetDeltaTime()`.

This is more convenient than using a `UEngineCustomTimeStep`, as a custom time step completely skips all the engine's built-in functionality in `UpdateTimeAndHandleMaxTickRate`. This plugin is supposed to complement most of that functionality, not replace it.

A better hook point might be via `ILatencyMarkerModule::SetSimulationLatencyMarkerStart`, as it is called immediately after `UpdateTimeAndHandleMaxTickRate()`, but I haven't looked into if that's needed yet.

## MaxDeltaTime

Unreal runs uncapped delta times by default, so very large hitches will have extreme recovery. By default, UE will simulate the next frame with an uncapped delta time matching the hitch in size to keep the game time constant, which can be jarring.

You can set `MaxDeltaTime` to something like 0.3 so that very large hitches don't have such extreme side effects, but your use case may vary.

> [!CAUTION]
> Be aware that clamping delta-time during bigger hitches causes audio to drift, as UE's audio runs on its own thread!  
> The Sequencer has audio-drifting correction built-in.

You can adjust `MaxDeltaTime` in your **DefaultEngine.ini**:

    [/Script/Engine.GameEngine]
    MaxDeltaTime=0.3
