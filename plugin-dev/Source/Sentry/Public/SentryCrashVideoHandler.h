// Copyright (c) 2025 Unreal Solutions Ltd. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "SentryCrashVideoHandler.generated.h"

/**
 * Handler class that manages automatic video recording for crash reports.
 * 
 * This class integrates Runtime Video Recorder with Sentry's crash reporting system.
 * When enabled, it continuously records gameplay in a circular buffer (last N seconds)
 * and automatically attaches the video to crash reports sent to Sentry.
 * 
 * Usage Example:
 * 
 * // In your game initialization (e.g., BeginPlay):
 * USentryCrashVideoHandler* VideoHandler = NewObject<USentryCrashVideoHandler>();
 * VideoHandler->StartContinuousRecording(30.0f); // Record last 30 seconds
 * 
 * // The video will automatically be attached to any crash reports
 * 
 * Features:
 * - Low performance overhead (uses circular buffer)
 * - Configurable duration (5 seconds to 10 minutes)
 * - Automatic cleanup of old videos
 * - Works with all Sentry crash types (native crashes, asserts, ensures)
 */

USTRUCT(BlueprintType)
struct FCrashVideoConfig
{
	GENERATED_BODY()

	/** Number of seconds to keep in the recording buffer (5-600 seconds) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Video")
	float LastSecondsToRecord = 30.0f;

	/** Frame rate for video recording (15-60 FPS recommended) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Video")
	int32 TargetFPS = 30;

	/** Video width in pixels (-1 for viewport width) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Video")
	int32 Width = 1280;

	/** Video height in pixels (-1 for viewport height) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Video")
	int32 Height = 720;

	/** Whether to include UI/widgets in the recording */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Video")
	bool bRecordUI = true;

	/** Whether to record audio (disabled by default for performance) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Video")
	bool bEnableAudio = false;

	/** Video quality preset (0-100, higher = better quality but larger file) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Video", meta = (ClampMin = "0", ClampMax = "100"))
	int32 QualityPreset = 50;
};


UCLASS(BlueprintType)
class SENTRY_API USentryCrashVideoHandler : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Configuration for crash video recording.
	 */

	/**
	 * Start continuous video recording for crash reports.
	 * Records in a circular buffer, keeping only the last N seconds.
	 * 
	 * @param Config - Configuration for video recording
	 * @return True if recording started successfully
	 */
	UFUNCTION(BlueprintCallable, Category = "Sentry|Video")
	bool StartContinuousRecording(const FCrashVideoConfig& Config);

	/**
	 * Start continuous recording with default settings.
	 * 
	 * @param LastSecondsToRecord - Duration to keep in buffer (default 30 seconds)
	 * @return True if recording started successfully
	 */
	UFUNCTION(BlueprintCallable, Category = "Sentry|Video")
	bool StartContinuousRecordingSimple(float LastSecondsToRecord = 30.0f);

	/**
	 * Stop continuous video recording.
	 */
	UFUNCTION(BlueprintCallable, Category = "Sentry|Video")
	void StopContinuousRecording();

	/**
	 * Check if continuous recording is active.
	 */
	UFUNCTION(BlueprintPure, Category = "Sentry|Video")
	bool IsRecording() const { return bIsRecording; }

	/**
	 * Get the directory where crash videos are stored.
	 */
	UFUNCTION(BlueprintPure, Category = "Sentry|Video")
	FString GetCrashVideoDirectory() const;

	/**
	 * Manually trigger a video save and attachment (useful for non-crash error reporting).
	 * This stops the current recording and attaches it to the next Sentry event.
	 * 
	 * @return Path to the saved video file
	 */
	UFUNCTION(BlueprintCallable, Category = "Sentry|Video")
	FString CaptureAndAttachVideo();

	/**
	 * Set maximum number of crash videos to keep on disk.
	 * Older videos will be automatically deleted.
	 */
	UFUNCTION(BlueprintCallable, Category = "Sentry|Video")
	void SetMaxVideosToKeep(int32 MaxVideos);

public:
	// Destructor
	virtual void BeginDestroy() override;

private:
	/**
	 * Initialize the crash detection hook.
	 */
	void InitializeCrashDetection();

	/**
	 * Called when a crash/assert is detected.
	 */
	void OnCrashDetected(const FString& ErrorMessage);

	/**
	 * Called before Sentry sends an event (crash or error).
	 * This is where we attach the video.
	 */
	void OnBeforeSentryEvent();

	/**
	 * Finalize the current recording and save to disk.
	 * @return Path to saved video, or empty if failed
	 */
	FString FinalizeAndSaveVideo();

	/**
	 * Attach video file to current Sentry scope.
	 */
	bool AttachVideoToSentry(const FString& VideoPath);

	/**
	 * Clean up old crash video files.
	 */
	void CleanupOldVideos();

	/**
	 * Generate a unique filename for crash video.
	 */
	FString GenerateVideoFilename() const;

private:
	bool bIsRecording = false;
	FCrashVideoConfig CurrentConfig;
	FString CurrentSessionVideoPath;
	int32 MaxVideosToKeep = 10;
	
	FDelegateHandle ErrorOutputDeviceDelegateHandle;
};

