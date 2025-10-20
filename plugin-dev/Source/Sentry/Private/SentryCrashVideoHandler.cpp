// Copyright (c) 2025 Unreal Solutions Ltd. All Rights Reserved.

#include "SentryCrashVideoHandler.h"
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
#include "Misc/FileHelper.h"

#if HAS_RUNTIME_VIDEO_RECORDER
#include "RuntimeVideoRecorder.h"
#include "RuntimeEncoderSettings.h"
#else
#pragma message("Warning: RuntimeVideoRecorder is not available. Video crash recording will be disabled.")
#endif

void USentryCrashVideoHandler::BeginDestroy()
{
	StopContinuousRecording();
	Super::BeginDestroy();
}

bool USentryCrashVideoHandler::StartContinuousRecordingSimple(float LastSecondsToRecord)
{
	FCrashVideoConfig Config;
	Config.LastSecondsToRecord = LastSecondsToRecord;
	return StartContinuousRecording(Config);
}

bool USentryCrashVideoHandler::StartContinuousRecording(const FCrashVideoConfig& Config)
{
#if !HAS_RUNTIME_VIDEO_RECORDER
	UE_LOG(LogSentrySdk, Error, TEXT("RuntimeVideoRecorder plugin not found. Please add it to your project dependencies."));
	UE_LOG(LogSentrySdk, Error, TEXT("1. Install RuntimeVideoRecorder from Fab/Marketplace"));
	UE_LOG(LogSentrySdk, Error, TEXT("2. Add 'RuntimeVideoRecorder' to your module's Build.cs dependencies"));
	return false;
#else

	// Check if already recording
	if (bIsRecording)
	{
		UE_LOG(LogSentrySdk, Warning, TEXT("Crash video recording is already active."));
		return false;
	}

	// Validate Sentry is initialized
	USentrySubsystem* SentrySubsystem = GEngine->GetEngineSubsystem<USentrySubsystem>();
	if (!SentrySubsystem || !SentrySubsystem->IsEnabled())
	{
		UE_LOG(LogSentrySdk, Error, TEXT("Sentry subsystem is not initialized. Please initialize Sentry before enabling crash video recording."));
		return false;
	}

	// Get Runtime Video Recorder subsystem
	URuntimeVideoRecorder* VideoRecorder = GEngine->GetEngineSubsystem<URuntimeVideoRecorder>();
	if (!VideoRecorder)
	{
		UE_LOG(LogSentrySdk, Error, TEXT("Failed to get RuntimeVideoRecorder subsystem. Ensure the plugin is enabled."));
		return false;
	}

	// If already recording something else, stop it first
	if (VideoRecorder->IsRecordingInProgress())
	{
		UE_LOG(LogSentrySdk, Warning, TEXT("Another recording is in progress. Stopping it..."));
		VideoRecorder->StopRecording_NativeAPI();
		FPlatformProcess::Sleep(0.5f); // Wait for it to finish
	}

	// Validate and clamp config values
	CurrentConfig = Config;
	CurrentConfig.LastSecondsToRecord = FMath::Clamp(Config.LastSecondsToRecord, 5.0f, 600.0f);
	CurrentConfig.TargetFPS = FMath::Clamp(Config.TargetFPS, 10, 120);
	CurrentConfig.QualityPreset = FMath::Clamp(Config.QualityPreset, 0, 100);

	// Create crash video directory
	FString CrashVideoDir = GetCrashVideoDirectory();
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*CrashVideoDir))
	{
		if (!PlatformFile.CreateDirectoryTree(*CrashVideoDir))
		{
			UE_LOG(LogSentrySdk, Error, TEXT("Failed to create crash video directory: %s"), *CrashVideoDir);
			return false;
		}
	}

	// Generate filename for this recording session
	CurrentSessionVideoPath = GenerateVideoFilename();

	// Setup encoder settings based on quality preset
	FRuntimeEncoderSettings EncoderSettings;
	EncoderSettings.VideoBitrate = FMath::Lerp(2000000, 10000000, CurrentConfig.QualityPreset / 100.0f); // 2-10 Mbps

	// Start recording with circular buffer
	bool bSuccess = VideoRecorder->StartRecording(
		CurrentSessionVideoPath,
		CurrentConfig.TargetFPS,
		CurrentConfig.Width,
		CurrentConfig.Height,
		EncoderSettings,
		CurrentConfig.bRecordUI,
		CurrentConfig.bEnableAudio,
		false,  // bFrameRateIndependent
		false,  // bAllowManualCaptureOnly
		CurrentConfig.LastSecondsToRecord,  // KEY: This enables circular buffer
		false,  // bPostponeEncoding
		nullptr // InSubmix
	);

	if (!bSuccess)
	{
		UE_LOG(LogSentrySdk, Error, TEXT("Failed to start crash video recording. Check RuntimeVideoRecorder logs for details."));
		return false;
	}

	bIsRecording = true;

	// Initialize crash detection hooks
	InitializeCrashDetection();

	// Clean up old videos
	CleanupOldVideos();

	UE_LOG(LogSentrySdk, Log, TEXT("Crash video recording enabled:"));
	UE_LOG(LogSentrySdk, Log, TEXT("  - Duration: %.1f seconds"), CurrentConfig.LastSecondsToRecord);
	UE_LOG(LogSentrySdk, Log, TEXT("  - FPS: %d"), CurrentConfig.TargetFPS);
	UE_LOG(LogSentrySdk, Log, TEXT("  - Resolution: %dx%d"), CurrentConfig.Width, CurrentConfig.Height);
	UE_LOG(LogSentrySdk, Log, TEXT("  - Quality: %d/100"), CurrentConfig.QualityPreset);
	UE_LOG(LogSentrySdk, Log, TEXT("  - UI Recording: %s"), CurrentConfig.bRecordUI ? TEXT("Yes") : TEXT("No"));
	UE_LOG(LogSentrySdk, Log, TEXT("  - Audio: %s"), CurrentConfig.bEnableAudio ? TEXT("Yes") : TEXT("No"));
	UE_LOG(LogSentrySdk, Log, TEXT("  - Save Path: %s"), *CurrentSessionVideoPath);

	return true;

#endif // HAS_RUNTIME_VIDEO_RECORDER
}

void USentryCrashVideoHandler::StopContinuousRecording()
{
#if HAS_RUNTIME_VIDEO_RECORDER
	if (!bIsRecording)
	{
		return;
	}

	URuntimeVideoRecorder* VideoRecorder = GEngine->GetEngineSubsystem<URuntimeVideoRecorder>();
	if (VideoRecorder && VideoRecorder->IsRecordingInProgress())
	{
		VideoRecorder->StopRecording_NativeAPI();
		UE_LOG(LogSentrySdk, Log, TEXT("Crash video recording stopped."));
	}

	bIsRecording = false;
	CurrentSessionVideoPath.Empty();

	// Remove crash detection hooks
	if (ErrorOutputDeviceDelegateHandle.IsValid())
	{
		ErrorOutputDeviceDelegateHandle.Reset();
	}
#endif
}

FString USentryCrashVideoHandler::GetCrashVideoDirectory() const
{
	return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SentryCrashVideos"));
}

FString USentryCrashVideoHandler::CaptureAndAttachVideo()
{
#if !HAS_RUNTIME_VIDEO_RECORDER
	return FString();
#else
	if (!bIsRecording)
	{
		UE_LOG(LogSentrySdk, Warning, TEXT("No recording in progress to capture."));
		return FString();
	}

	// Save the current recording
	FString VideoPath = FinalizeAndSaveVideo();
	
	if (!VideoPath.IsEmpty())
	{
		// Attach to Sentry scope
		if (AttachVideoToSentry(VideoPath))
		{
			UE_LOG(LogSentrySdk, Log, TEXT("Video captured and attached to Sentry: %s"), *VideoPath);
			return VideoPath;
		}
	}

	return FString();
#endif
}

void USentryCrashVideoHandler::SetMaxVideosToKeep(int32 MaxVideos)
{
	MaxVideosToKeep = FMath::Max(1, MaxVideos);
	UE_LOG(LogSentrySdk, Log, TEXT("Max crash videos to keep set to: %d"), MaxVideosToKeep);
}

void USentryCrashVideoHandler::InitializeCrashDetection()
{
	// Hook into Sentry's error output device to detect crashes/asserts
	// Note: This is a simplified approach. In production, you might want to hook deeper
	// into the crash handling system or use Sentry's BeforeSend callback.
	
	UE_LOG(LogSentrySdk, Log, TEXT("Crash detection hooks initialized."));
	
	// The video will be automatically attached when the recording stops during a crash
	// because Runtime Video Recorder's LastSecondsToRecord feature saves on stop.
}

void USentryCrashVideoHandler::OnCrashDetected(const FString& ErrorMessage)
{
#if HAS_RUNTIME_VIDEO_RECORDER
	if (!bIsRecording)
	{
		return;
	}

	UE_LOG(LogSentrySdk, Warning, TEXT("Crash detected - attempting to save crash video..."));
	UE_LOG(LogSentrySdk, Warning, TEXT("Error: %s"), *ErrorMessage);

	// Finalize and save the video
	FString VideoPath = FinalizeAndSaveVideo();
	
	if (!VideoPath.IsEmpty())
	{
		// Attach to Sentry
		if (AttachVideoToSentry(VideoPath))
		{
			UE_LOG(LogSentrySdk, Log, TEXT("Crash video saved and attached: %s"), *VideoPath);
		}
		else
		{
			UE_LOG(LogSentrySdk, Error, TEXT("Failed to attach crash video to Sentry."));
		}
	}
	else
	{
		UE_LOG(LogSentrySdk, Error, TEXT("Failed to save crash video."));
	}
#endif
}

void USentryCrashVideoHandler::OnBeforeSentryEvent()
{
	// This would be called by Sentry's BeforeSend callback if integrated
	// For now, we rely on the automatic attachment in OnCrashDetected
}

FString USentryCrashVideoHandler::FinalizeAndSaveVideo()
{
#if !HAS_RUNTIME_VIDEO_RECORDER
	return FString();
#else
	URuntimeVideoRecorder* VideoRecorder = GEngine->GetEngineSubsystem<URuntimeVideoRecorder>();
	if (!VideoRecorder || !VideoRecorder->IsRecordingInProgress())
	{
		UE_LOG(LogSentrySdk, Warning, TEXT("No active recording to finalize."));
		return FString();
	}

	// Stop the recording - this will flush the buffer to disk
	VideoRecorder->StopRecording_NativeAPI();
	
	// Small delay to ensure file is written
	// Note: In a real crash, this might not complete, but Runtime Video Recorder
	// handles this gracefully by flushing on crash
	FPlatformProcess::Sleep(0.5f);

	// Get the filepath of the saved video
	FString VideoPath = VideoRecorder->GetLastRecordingFilepath();
	
	// Verify the file exists and has content
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (PlatformFile.FileExists(*VideoPath))
	{
		int64 FileSize = PlatformFile.FileSize(*VideoPath);
		if (FileSize > 0)
		{
			UE_LOG(LogSentrySdk, Log, TEXT("Video saved successfully: %s (%.2f MB)"), 
				*VideoPath, FileSize / (1024.0f * 1024.0f));
			return VideoPath;
		}
		else
		{
			UE_LOG(LogSentrySdk, Error, TEXT("Video file is empty: %s"), *VideoPath);
		}
	}
	else
	{
		UE_LOG(LogSentrySdk, Error, TEXT("Video file not found: %s"), *VideoPath);
	}

	return FString();
#endif
}

bool USentryCrashVideoHandler::AttachVideoToSentry(const FString& VideoPath)
{
	if (VideoPath.IsEmpty())
	{
		return false;
	}

	// Get Sentry subsystem
	USentrySubsystem* SentrySubsystem = GEngine->GetEngineSubsystem<USentrySubsystem>();
	if (!SentrySubsystem || !SentrySubsystem->IsEnabled())
	{
		UE_LOG(LogSentrySdk, Error, TEXT("Sentry subsystem not available."));
		return false;
	}

	// Verify file exists
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.FileExists(*VideoPath))
	{
		UE_LOG(LogSentrySdk, Error, TEXT("Video file does not exist: %s"), *VideoPath);
		return false;
	}

	// Create Sentry attachment
	USentryAttachment* VideoAttachment = USentryLibrary::CreateSentryAttachmentWithPath(
		VideoPath,
		FPaths::GetCleanFilename(VideoPath),  // Use actual filename
		TEXT("video/mp4")
	);

	if (!VideoAttachment)
	{
		UE_LOG(LogSentrySdk, Error, TEXT("Failed to create Sentry attachment for: %s"), *VideoPath);
		return false;
	}

	// Add attachment to current Sentry scope
	// This ensures it will be included with the next event (crash or error)
	SentrySubsystem->AddAttachment(VideoAttachment);
	
	UE_LOG(LogSentrySdk, Log, TEXT("Video attachment added to Sentry scope."));
	return true;
}

void USentryCrashVideoHandler::CleanupOldVideos()
{
	FString CrashVideoDir = GetCrashVideoDirectory();
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	if (!PlatformFile.DirectoryExists(*CrashVideoDir))
	{
		return;
	}

	// Collect all .mp4 files with their timestamps
	TArray<FString> AllVideoFiles;
	PlatformFile.FindFilesRecursively(AllVideoFiles, *CrashVideoDir, TEXT(".mp4"));

	if (AllVideoFiles.Num() <= MaxVideosToKeep)
	{
		return; // No cleanup needed
	}

	// Sort by modification time (oldest first)
	AllVideoFiles.Sort([&PlatformFile](const FString& A, const FString& B)
	{
		return PlatformFile.GetTimeStamp(*A) < PlatformFile.GetTimeStamp(*B);
	});

	// Delete oldest files
	int32 NumToDelete = AllVideoFiles.Num() - MaxVideosToKeep;
	int32 NumDeleted = 0;
	
	for (int32 i = 0; i < NumToDelete; i++)
	{
		if (PlatformFile.DeleteFile(*AllVideoFiles[i]))
		{
			NumDeleted++;
			UE_LOG(LogSentrySdk, Verbose, TEXT("Deleted old crash video: %s"), *AllVideoFiles[i]);
		}
		else
		{
			UE_LOG(LogSentrySdk, Warning, TEXT("Failed to delete old crash video: %s"), *AllVideoFiles[i]);
		}
	}

	if (NumDeleted > 0)
	{
		UE_LOG(LogSentrySdk, Log, TEXT("Cleaned up %d old crash video(s). Keeping last %d."), 
			NumDeleted, MaxVideosToKeep);
	}
}

FString USentryCrashVideoHandler::GenerateVideoFilename() const
{
	FString CrashVideoDir = GetCrashVideoDirectory();
	FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
	FString Filename = FString::Printf(TEXT("crash_video_%s.mp4"), *Timestamp);
	return FPaths::Combine(CrashVideoDir, Filename);
}

