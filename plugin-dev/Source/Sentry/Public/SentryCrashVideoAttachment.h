// Copyright (c) 2025 Unreal Solutions Ltd. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/EngineSubsystem.h"
#include "SentryCrashVideoAttachment.generated.h"

/**
 * Subsystem that integrates Runtime Video Recorder with Sentry crash reporting.
 * Automatically records last N seconds of gameplay and attaches video to crash reports.
 */
UCLASS()
class SENTRY_API USentryCrashVideoAttachment : public UEngineSubsystem
{
	GENERATED_BODY()

public:
	/**
	 * Enable automatic video recording for crash reports.
	 * This will continuously record the last N seconds of gameplay.
	 * 
	 * @param LastSecondsToRecord - Number of seconds to keep in buffer (default 30 seconds)
	 * @param TargetFPS - Frame rate for video recording (default 30)
	 * @param Width - Video width (default 1280, -1 for viewport width)
	 * @param Height - Video height (default 720, -1 for viewport height)
	 * @param bRecordUI - Whether to include UI in the recording (default true)
	 * @param bEnableAudioRecording - Whether to record audio (default false, disabled for performance)
	 */
	UFUNCTION(BlueprintCallable, Category = "Sentry|Video")
	void EnableCrashVideoRecording(
		float LastSecondsToRecord = 30.0f,
		int32 TargetFPS = 30,
		int32 Width = 1280,
		int32 Height = 720,
		bool bRecordUI = true,
		bool bEnableAudioRecording = false
	);

	/**
	 * Disable automatic video recording for crash reports.
	 */
	UFUNCTION(BlueprintCallable, Category = "Sentry|Video")
	void DisableCrashVideoRecording();

	/**
	 * Check if crash video recording is currently active.
	 */
	UFUNCTION(BlueprintPure, Category = "Sentry|Video")
	bool IsCrashVideoRecordingEnabled() const { return bIsVideoRecordingEnabled; }

	/**
	 * Get the path where crash videos will be saved.
	 */
	UFUNCTION(BlueprintPure, Category = "Sentry|Video")
	FString GetCrashVideoDirectory() const;

protected:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

private:
	/**
	 * Called when a crash or assert occurs.
	 * Finalizes the video recording and attaches it to the Sentry crash report.
	 */
	void OnCrashDetected(const FString& Message);

	/**
	 * Saves the current video buffer to disk.
	 * @return Path to the saved video file, or empty string if save failed.
	 */
	FString SaveCrashVideo();

	/**
	 * Attaches the video file to the current Sentry scope.
	 */
	void AttachVideoToSentry(const FString& VideoPath);

	/**
	 * Cleans up old crash video files to prevent disk space issues.
	 */
	void CleanupOldCrashVideos();

private:
	bool bIsVideoRecordingEnabled = false;
	FString CrashVideoPath;
	float RecordingDuration = 30.0f;
	
	// Delegate handle for crash detection
	FDelegateHandle OnAssertDelegateHandle;
};

