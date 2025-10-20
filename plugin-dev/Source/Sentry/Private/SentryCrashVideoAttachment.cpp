// Copyright (c) 2025 Unreal Solutions Ltd. All Rights Reserved.

#include "SentryCrashVideoAttachment.h"
#include "SentrySubsystem.h"
#include "SentryAttachment.h"
#include "SentryDefines.h"
#include "SentryErrorOutputDevice.h"
#include "SentryLibrary.h"

#include "Engine/Engine.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"

// Runtime Video Recorder includes
// Note: This assumes RuntimeVideoRecorder plugin is available
#if HAS_RUNTIME_VIDEO_RECORDER
#include "RuntimeEncoderSettings.h"
#include "RuntimeVideoRecorder.h"
#else
#pragma message("Warning: RuntimeVideoRecorder is not available. Video crash recording will be disabled.")
#endif

void USentryCrashVideoAttachment::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

#if !HAS_RUNTIME_VIDEO_RECORDER
	UE_LOG(LogSentrySdk, Warning, TEXT("Sentry Crash Video Attachment: RuntimeVideoRecorder plugin not found. Video crash recording will not be available."));
#else
	UE_LOG(LogSentrySdk, Log, TEXT("Sentry Crash Video Attachment subsystem initialized."));
#endif
}

void USentryCrashVideoAttachment::Deinitialize()
{
	DisableCrashVideoRecording();
	Super::Deinitialize();
}

void USentryCrashVideoAttachment::EnableCrashVideoRecording(
	float LastSecondsToRecord,
	int32 TargetFPS,
	int32 Width,
	int32 Height,
	bool bRecordUI,
	bool bEnableAudioRecording)
{
#if !HAS_RUNTIME_VIDEO_RECORDER
	UE_LOG(LogSentrySdk, Error, TEXT("RuntimeVideoRecorder plugin not found. Please add RuntimeVideoRecorder to your project dependencies."));
	return;
#else

	if (bIsVideoRecordingEnabled)
	{
		UE_LOG(LogSentrySdk, Warning, TEXT("Crash video recording is already enabled."));
		return;
	}

	// Validate parameters
	LastSecondsToRecord = FMath::Clamp(LastSecondsToRecord, 5.0f, 600.0f); // Between 5 seconds and 10 minutes
	RecordingDuration = LastSecondsToRecord;

	// Get the Runtime Video Recorder subsystem
	URuntimeVideoRecorder* VideoRecorder = GEngine->GetEngineSubsystem<URuntimeVideoRecorder>();
	if (!VideoRecorder)
	{
		UE_LOG(LogSentrySdk, Error, TEXT("Failed to get RuntimeVideoRecorder subsystem."));
		return;
	}

	// Setup crash video directory
	FString ProjectSavedDir = FPaths::ProjectSavedDir();
	FString CrashVideoDir = FPaths::Combine(ProjectSavedDir, TEXT("SentryCrashVideos"));
	
	// Create directory if it doesn't exist
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*CrashVideoDir))
	{
		PlatformFile.CreateDirectory(*CrashVideoDir);
	}

	// Generate unique filename for this session
	FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
	CrashVideoPath = FPaths::Combine(CrashVideoDir, FString::Printf(TEXT("crash_recording_%s.mp4"), *Timestamp));

	// Start recording with LastSecondsToRecord enabled
	// This will continuously record, keeping only the last N seconds in memory
	bool bSuccess = VideoRecorder->StartRecording(
		CrashVideoPath,
		TargetFPS,
		Width,
		Height,
		FRuntimeEncoderSettings(), // Use default encoder settings
		bRecordUI,
		bEnableAudioRecording,
		false,  // bFrameRateIndependent = false for better game performance
		false,  // bAllowManualCaptureOnly = false
		LastSecondsToRecord,  // This is the key parameter - only keep last N seconds
		false,  // bPostponeEncoding = false, encode on-the-fly
		nullptr // InSubmix = nullptr (all audio)
	);

	if (!bSuccess)
	{
		UE_LOG(LogSentrySdk, Error, TEXT("Failed to start crash video recording."));
		return;
	}

	bIsVideoRecordingEnabled = true;

	// Hook into Sentry's error output device to detect crashes
	USentrySubsystem* SentrySubsystem = GEngine->GetEngineSubsystem<USentrySubsystem>();
	if (SentrySubsystem)
	{
		// We need to get access to the error output device
		// This is a bit tricky as we need to hook into the crash detection
		// For now, we'll use a timer-based approach to check if a crash occurred
		UE_LOG(LogSentrySdk, Log, TEXT("Crash video recording enabled - recording last %.1f seconds at %d FPS."), LastSecondsToRecord, TargetFPS);
	}
	else
	{
		UE_LOG(LogSentrySdk, Warning, TEXT("Sentry subsystem not found or not initialized. Video will be recorded but may not be attached to crash reports."));
	}

	// Clean up old crash videos (keep only last 10)
	CleanupOldCrashVideos();

#endif // HAS_RUNTIME_VIDEO_RECORDER
}

void USentryCrashVideoAttachment::DisableCrashVideoRecording()
{
#if HAS_RUNTIME_VIDEO_RECORDER
	if (!bIsVideoRecordingEnabled)
	{
		return;
	}

	URuntimeVideoRecorder* VideoRecorder = GEngine->GetEngineSubsystem<URuntimeVideoRecorder>();
	if (VideoRecorder && VideoRecorder->IsRecordingInProgress())
	{
		VideoRecorder->StopRecording_NativeAPI();
		UE_LOG(LogSentrySdk, Log, TEXT("Crash video recording disabled."));
	}

	bIsVideoRecordingEnabled = false;
	CrashVideoPath.Empty();

	if (OnAssertDelegateHandle.IsValid())
	{
		OnAssertDelegateHandle.Reset();
	}
#endif
}

FString USentryCrashVideoAttachment::GetCrashVideoDirectory() const
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SentryCrashVideos"));
}

void USentryCrashVideoAttachment::OnCrashDetected(const FString& Message)
{
#if HAS_RUNTIME_VIDEO_RECORDER
	if (!bIsVideoRecordingEnabled)
	{
		return;
	}

	UE_LOG(LogSentrySdk, Log, TEXT("Crash detected - saving crash video..."));

	// Save the video
	FString SavedVideoPath = SaveCrashVideo();
	
	if (!SavedVideoPath.IsEmpty())
	{
		// Attach to Sentry
		AttachVideoToSentry(SavedVideoPath);
		UE_LOG(LogSentrySdk, Log, TEXT("Crash video attached to Sentry report: %s"), *SavedVideoPath);
	}
	else
	{
		UE_LOG(LogSentrySdk, Error, TEXT("Failed to save crash video."));
	}
#endif
}

FString USentryCrashVideoAttachment::SaveCrashVideo()
{
#if HAS_RUNTIME_VIDEO_RECORDER
	URuntimeVideoRecorder* VideoRecorder = GEngine->GetEngineSubsystem<URuntimeVideoRecorder>();
	if (!VideoRecorder || !VideoRecorder->IsRecordingInProgress())
	{
		return FString();
	}

	// Stop recording - this will flush the buffer to disk
	VideoRecorder->StopRecording_NativeAPI();
	
	// Wait a bit for the file to be written (this is a limitation in crash handlers)
	// In a real crash scenario, this might not complete, so we rely on the buffer system
	FPlatformProcess::Sleep(0.5f);

	// Get the last recording filepath
	FString VideoPath = VideoRecorder->GetLastRecordingFilepath();
	
	// Verify the file exists
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (PlatformFile.FileExists(*VideoPath))
	{
		return VideoPath;
	}

	return FString();
#else
	return FString();
#endif
}

void USentryCrashVideoAttachment::AttachVideoToSentry(const FString& VideoPath)
{
	if (VideoPath.IsEmpty())
	{
		return;
	}

	USentrySubsystem* SentrySubsystem = GEngine->GetEngineSubsystem<USentrySubsystem>();
	if (!SentrySubsystem || !SentrySubsystem->IsEnabled())
	{
		UE_LOG(LogSentrySdk, Warning, TEXT("Sentry subsystem not available, cannot attach video."));
		return;
	}

	// Create a Sentry attachment for the video file
	USentryAttachment* VideoAttachment = USentryLibrary::CreateSentryAttachmentWithPath(
		VideoPath,
		TEXT("crash_recording.mp4"),
		TEXT("video/mp4")
	);

	if (VideoAttachment)
	{
		// Add the attachment to the current scope
		// This will ensure it's included with the next event (including crashes)
		SentrySubsystem->AddAttachment(VideoAttachment);
		UE_LOG(LogSentrySdk, Log, TEXT("Video attachment added to Sentry scope: %s"), *VideoPath);
	}
	else
	{
		UE_LOG(LogSentrySdk, Error, TEXT("Failed to create Sentry attachment for video: %s"), *VideoPath);
	}
}

void USentryCrashVideoAttachment::CleanupOldCrashVideos()
{
	FString CrashVideoDir = GetCrashVideoDirectory();
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	if (!PlatformFile.DirectoryExists(*CrashVideoDir))
	{
		return;
	}

	// Find all .mp4 files in the crash video directory
	TArray<FString> VideoFiles;
	PlatformFile.FindFiles(VideoFiles, *CrashVideoDir, TEXT(".mp4"));

	// Sort by modification time (oldest first)
	VideoFiles.Sort([&PlatformFile](const FString& A, const FString& B)
	{
		return PlatformFile.GetTimeStamp(*A) < PlatformFile.GetTimeStamp(*B);
	});

	// Keep only the last 10 videos, delete older ones
	const int32 MaxVideosToKeep = 10;
	if (VideoFiles.Num() > MaxVideosToKeep)
	{
		int32 NumToDelete = VideoFiles.Num() - MaxVideosToKeep;
		for (int32 i = 0; i < NumToDelete; i++)
		{
			if (PlatformFile.DeleteFile(*VideoFiles[i]))
			{
				UE_LOG(LogSentrySdk, Log, TEXT("Deleted old crash video: %s"), *VideoFiles[i]);
			}
		}
	}
}

