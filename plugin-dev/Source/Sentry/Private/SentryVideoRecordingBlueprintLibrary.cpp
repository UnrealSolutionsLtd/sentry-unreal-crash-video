// Copyright (c) 2025 Unreal Solutions Ltd. All Rights Reserved.

#include "SentryVideoRecordingBlueprintLibrary.h"
#include "SentrySubsystem.h"
#include "SentryDefines.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"

// Runtime Video Recorder check
#if HAS_RUNTIME_VIDEO_RECORDER
#include "RuntimeEncoderSettings.h"
#include "RuntimeVideoRecorder.h"
#else
#pragma message("Warning: RuntimeVideoRecorder is not available. Video crash recording will be disabled.")
#endif

// Property name for storing the video handler in the game instance
static const FName VideoHandlerPropertyName = TEXT("SentryCrashVideoHandler_Internal");

USentryCrashVideoHandler* USentryVideoRecordingBlueprintLibrary::GetOrCreateVideoHandler(UObject* WorldContextObject)
{
	if (!WorldContextObject)
	{
		UE_LOG(LogSentrySdk, Error, TEXT("WorldContextObject is null"));
		return nullptr;
	}

	UGameInstance* GameInstance = WorldContextObject->GetWorld()->GetGameInstance();
	if (!GameInstance)
	{
		UE_LOG(LogSentrySdk, Error, TEXT("GameInstance is null"));
		return nullptr;
	}

	// Try to get existing handler
	USentryCrashVideoHandler* ExistingHandler = GetExistingVideoHandler(WorldContextObject);
	if (ExistingHandler)
	{
		return ExistingHandler;
	}

	// Create new handler and store it
	USentryCrashVideoHandler* NewHandler = NewObject<USentryCrashVideoHandler>(GameInstance);
	return NewHandler;
}

USentryCrashVideoHandler* USentryVideoRecordingBlueprintLibrary::GetExistingVideoHandler(UObject* WorldContextObject)
{
	if (!WorldContextObject)
	{
		return nullptr;
	}

	UGameInstance* GameInstance = WorldContextObject->GetWorld()->GetGameInstance();
	if (!GameInstance)
	{
		return nullptr;
	}

	// Try to find existing handler in game instance's outer objects
	TArray<UObject*> Objects;
	GetObjectsWithOuter(GameInstance, Objects, false);
	
	for (UObject* Obj : Objects)
	{
		if (USentryCrashVideoHandler* Handler = Cast<USentryCrashVideoHandler>(Obj))
		{
			return Handler;
		}
	}

	return nullptr;
}

bool USentryVideoRecordingBlueprintLibrary::SentryEnableCrashVideoRecording(
	UObject* WorldContextObject,
	float LastSecondsToRecord)
{
	// Check prerequisites
	if (!SentryIsVideoRecordingAvailable())
	{
		UE_LOG(LogSentrySdk, Error, TEXT("Sentry or RuntimeVideoRecorder not available. Cannot enable crash video recording."));
		return false;
	}

	// Get or create handler
	USentryCrashVideoHandler* VideoHandler = GetOrCreateVideoHandler(WorldContextObject);
	if (!VideoHandler)
	{
		UE_LOG(LogSentrySdk, Error, TEXT("Failed to create video handler"));
		return false;
	}

	// Start recording
	bool bSuccess = VideoHandler->StartContinuousRecordingSimple(LastSecondsToRecord);
	
	if (bSuccess)
	{
		UE_LOG(LogSentrySdk, Log, TEXT("Crash video recording enabled via Blueprint"));
	}

	return bSuccess;
}

bool USentryVideoRecordingBlueprintLibrary::SentryEnableCrashVideoRecordingAdvanced(
	UObject* WorldContextObject,
	const FCrashVideoConfig& Config)
{
	if (!SentryIsVideoRecordingAvailable())
	{
		UE_LOG(LogSentrySdk, Error, TEXT("Sentry or RuntimeVideoRecorder not available"));
		return false;
	}

	USentryCrashVideoHandler* VideoHandler = GetOrCreateVideoHandler(WorldContextObject);
	if (!VideoHandler)
	{
		return false;
	}

	return VideoHandler->StartContinuousRecording(Config);
}

void USentryVideoRecordingBlueprintLibrary::SentryDisableCrashVideoRecording(UObject* WorldContextObject)
{
	USentryCrashVideoHandler* VideoHandler = GetExistingVideoHandler(WorldContextObject);
	if (VideoHandler)
	{
		VideoHandler->StopContinuousRecording();
		UE_LOG(LogSentrySdk, Log, TEXT("Crash video recording disabled via Blueprint"));
	}
}

bool USentryVideoRecordingBlueprintLibrary::SentryIsCrashVideoRecordingActive(UObject* WorldContextObject)
{
	USentryCrashVideoHandler* VideoHandler = GetExistingVideoHandler(WorldContextObject);
	return VideoHandler ? VideoHandler->IsRecording() : false;
}

FString USentryVideoRecordingBlueprintLibrary::SentryCaptureVideoNow(UObject* WorldContextObject)
{
	USentryCrashVideoHandler* VideoHandler = GetExistingVideoHandler(WorldContextObject);
	if (!VideoHandler)
	{
		UE_LOG(LogSentrySdk, Warning, TEXT("No active video handler found"));
		return FString();
	}

	return VideoHandler->CaptureAndAttachVideo();
}

bool USentryVideoRecordingBlueprintLibrary::SentryEnableCrashVideoRecordingMobile(UObject* WorldContextObject)
{
	// Optimized settings for mobile devices
	FCrashVideoConfig Config;
	Config.LastSecondsToRecord = 15.0f;  // Shorter duration
	Config.TargetFPS = 20;                // Lower FPS
	Config.Width = 1280;                  // 720p
	Config.Height = 720;
	Config.bRecordUI = true;
	Config.bEnableAudio = false;          // No audio on mobile
	Config.QualityPreset = 30;            // Lower quality

	return SentryEnableCrashVideoRecordingAdvanced(WorldContextObject, Config);
}

bool USentryVideoRecordingBlueprintLibrary::SentryEnableCrashVideoRecordingPC(UObject* WorldContextObject)
{
	// Optimized settings for PC/Console
	FCrashVideoConfig Config;
	Config.LastSecondsToRecord = 30.0f;  // Standard duration
	Config.TargetFPS = 30;                // Standard FPS
	Config.Width = 1920;                  // 1080p
	Config.Height = 1080;
	Config.bRecordUI = true;
	Config.bEnableAudio = false;
	Config.QualityPreset = 50;            // Medium quality

	return SentryEnableCrashVideoRecordingAdvanced(WorldContextObject, Config);
}

bool USentryVideoRecordingBlueprintLibrary::SentryIsVideoRecordingAvailable()
{
#if !HAS_RUNTIME_VIDEO_RECORDER
	return false;
#else
	// Check if Sentry is available and initialized
	USentrySubsystem* SentrySubsystem = GEngine->GetEngineSubsystem<USentrySubsystem>();
	if (!SentrySubsystem || !SentrySubsystem->IsEnabled())
	{
		return false;
	}

	// Check if Runtime Video Recorder is available
	URuntimeVideoRecorder* VideoRecorder = GEngine->GetEngineSubsystem<URuntimeVideoRecorder>();
	if (!VideoRecorder)
	{
		return false;
	}

	return true;
#endif
}

FString USentryVideoRecordingBlueprintLibrary::SentryGetCrashVideoDirectory()
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SentryCrashVideos"));
}

