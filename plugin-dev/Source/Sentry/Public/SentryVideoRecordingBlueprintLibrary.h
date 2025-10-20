// Copyright (c) 2025 Unreal Solutions Ltd. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "SentryCrashVideoHandler.h"
#include "SentryVideoRecordingBlueprintLibrary.generated.h"

/**
 * Blueprint function library for easy integration of crash video recording with Sentry.
 * Provides simple one-line functions to enable crash video recording in your game.
 * 
 * Example Usage (Blueprint):
 * 
 *   On BeginPlay:
 *     └─ Sentry Enable Crash Video Recording (30 seconds)
 * 
 * That's it! Videos will automatically be attached to crash reports.
 */
UCLASS()
class SENTRY_API USentryVideoRecordingBlueprintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Enable automatic crash video recording with default settings.
	 * This is the simplest way to enable crash video recording.
	 * 
	 * @param WorldContextObject - World context (usually Self)
	 * @param LastSecondsToRecord - Number of seconds to keep in buffer (default 30)
	 * @return True if recording started successfully
	 * 
	 * Example (Blueprint):
	 *   Event BeginPlay → Sentry Enable Crash Video Recording (30.0)
	 */
	UFUNCTION(BlueprintCallable, Category = "Sentry|Video", meta = (WorldContext = "WorldContextObject"))
	static bool SentryEnableCrashVideoRecording(
		UObject* WorldContextObject,
		float LastSecondsToRecord = 30.0f
	);

	/**
	 * Enable crash video recording with custom settings.
	 * 
	 * @param WorldContextObject - World context (usually Self)
	 * @param Config - Custom configuration for video recording
	 * @return True if recording started successfully
	 */
	UFUNCTION(BlueprintCallable, Category = "Sentry|Video", meta = (WorldContext = "WorldContextObject"))
	static bool SentryEnableCrashVideoRecordingAdvanced(
		UObject* WorldContextObject,
		const FCrashVideoConfig& Config
	);

	/**
	 * Disable crash video recording.
	 * 
	 * @param WorldContextObject - World context (usually Self)
	 */
	UFUNCTION(BlueprintCallable, Category = "Sentry|Video", meta = (WorldContext = "WorldContextObject"))
	static void SentryDisableCrashVideoRecording(UObject* WorldContextObject);

	/**
	 * Check if crash video recording is currently active.
	 * 
	 * @param WorldContextObject - World context (usually Self)
	 * @return True if recording is active
	 */
	UFUNCTION(BlueprintPure, Category = "Sentry|Video", meta = (WorldContext = "WorldContextObject"))
	static bool SentryIsCrashVideoRecordingActive(UObject* WorldContextObject);

	/**
	 * Manually capture and attach the current video buffer to the next Sentry event.
	 * Useful for non-crash errors where you want to include video context.
	 * 
	 * @param WorldContextObject - World context (usually Self)
	 * @return Path to the saved video file, or empty if failed
	 * 
	 * Example (Blueprint):
	 *   1. Sentry Capture Video Now
	 *   2. Sentry Capture Message ("Error with video context")
	 */
	UFUNCTION(BlueprintCallable, Category = "Sentry|Video", meta = (WorldContext = "WorldContextObject"))
	static FString SentryCaptureVideoNow(UObject* WorldContextObject);

	/**
	 * Quick setup for mobile devices (lower settings for better performance).
	 * 
	 * @param WorldContextObject - World context (usually Self)
	 * @return True if recording started successfully
	 */
	UFUNCTION(BlueprintCallable, Category = "Sentry|Video", meta = (WorldContext = "WorldContextObject"))
	static bool SentryEnableCrashVideoRecordingMobile(UObject* WorldContextObject);

	/**
	 * Quick setup for PC/Console (higher quality settings).
	 * 
	 * @param WorldContextObject - World context (usually Self)
	 * @return True if recording started successfully
	 */
	UFUNCTION(BlueprintCallable, Category = "Sentry|Video", meta = (WorldContext = "WorldContextObject"))
	static bool SentryEnableCrashVideoRecordingPC(UObject* WorldContextObject);

	/**
	 * Check if Sentry and Runtime Video Recorder are both available.
	 * Use this before enabling crash video recording to verify prerequisites.
	 * 
	 * @return True if both plugins are available and initialized
	 */
	UFUNCTION(BlueprintPure, Category = "Sentry|Video")
	static bool SentryIsVideoRecordingAvailable();

	/**
	 * Get the directory where crash videos are stored.
	 * 
	 * @return Full path to crash video directory
	 */
	UFUNCTION(BlueprintPure, Category = "Sentry|Video")
	static FString SentryGetCrashVideoDirectory();

private:
	/**
	 * Get or create the video handler for the given world.
	 * Stores handler in the game instance to prevent garbage collection.
	 */
	static USentryCrashVideoHandler* GetOrCreateVideoHandler(UObject* WorldContextObject);

	/**
	 * Get existing video handler if it exists.
	 */
	static USentryCrashVideoHandler* GetExistingVideoHandler(UObject* WorldContextObject);
};

