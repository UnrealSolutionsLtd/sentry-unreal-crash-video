// Copyright (c) 2025 Unreal Solutions Ltd. All Rights Reserved.

#include "SentryCrashVideoHandler.h"
#include "SentrySubsystem.h"
#include "SentryAttachment.h"
#include "SentryDefines.h"
#include "SentryLibrary.h"
#include "SentryModule.h"

#include "Engine/Engine.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformProcess.h"

#if HAS_RUNTIME_VIDEO_RECORDER
#include "RuntimeVideoRecorder.h"
#include "RuntimeEncoderSettings.h"
#else
#pragma message("Warning: RuntimeVideoRecorder is not available. Video crash recording will be disabled.")
#endif

void USentryCrashVideoHandler::BeginDestroy()
{
	// Clean up metadata file if normal shutdown
	if (!bCrashDetected)
	{
		RemoveCrashMetadataFile();
	}
	
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
	bCrashDetected = false;
	
	// Check for and recover any videos from previous crashes FIRST
	// (before creating new metadata to avoid recovering our own file)
	RecoverPreviousCrashVideos();
	
	// Create metadata file for crash recovery
	CreateCrashMetadataFile();
	
	// Note: We don't pre-attach the video here because:
	// 1. The actual crash video file doesn't exist yet (circular buffer in memory)
	// 2. When crash occurs, TryCaptureEmergencyCrashVideo() will encode and attach it
	// 3. The crash recovery path has "_crash_recovery.mp4" suffix which differs from CurrentSessionVideoPath

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
	
	// Remove metadata file on normal stop
	if (!bCrashDetected)
	{
		RemoveCrashMetadataFile();
	}
	
	CurrentSessionVideoPath.Empty();
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

void USentryCrashVideoHandler::PreAttachVideoToSentry()
{
	if (CurrentSessionVideoPath.IsEmpty())
	{
		UE_LOG(LogSentrySdk, Warning, TEXT("Cannot pre-attach video: path is empty."));
		return;
	}

	USentrySubsystem* SentrySubsystem = GEngine->GetEngineSubsystem<USentrySubsystem>();
	if (!SentrySubsystem || !SentrySubsystem->IsEnabled())
	{
		UE_LOG(LogSentrySdk, Warning, TEXT("Sentry subsystem not available for pre-attachment."));
		return;
	}

	// Pre-create attachment object for the video file
	// This ensures Sentry knows about the video even if finalization fails
	USentryAttachment* VideoAttachment = USentryLibrary::CreateSentryAttachmentWithPath(
		CurrentSessionVideoPath,
		FPaths::GetCleanFilename(CurrentSessionVideoPath),
		TEXT("video/mp4")
	);

	if (VideoAttachment)
	{
		SentrySubsystem->AddAttachment(VideoAttachment);
		bVideoPreAttached = true;
		UE_LOG(LogSentrySdk, Log, TEXT("Video pre-attached to Sentry scope: %s"), *CurrentSessionVideoPath);
		UE_LOG(LogSentrySdk, Log, TEXT("If a crash occurs, this video will be included in the report."));
	}
	else
	{
		UE_LOG(LogSentrySdk, Warning, TEXT("Failed to pre-attach video to Sentry."));
	}
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

FString USentryCrashVideoHandler::GetMetadataFilePath() const
{
	if (CurrentSessionVideoPath.IsEmpty())
	{
		FString CrashVideoDir = GetCrashVideoDirectory();
		FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
		return FPaths::Combine(CrashVideoDir, FString::Printf(TEXT("crash_video_%s.meta"), *Timestamp));
	}
	
	// Generate metadata path based on video path
	FString BasePath = FPaths::GetPath(CurrentSessionVideoPath);
	FString BaseFilename = FPaths::GetBaseFilename(CurrentSessionVideoPath);
	return FPaths::Combine(BasePath, BaseFilename + TEXT(".meta"));
}

void USentryCrashVideoHandler::CreateCrashMetadataFile()
{
	if (CurrentSessionVideoPath.IsEmpty())
	{
		return;
	}

	FString MetadataPath = GetMetadataFilePath();
	
	// Create metadata with recording information
	FString MetadataContent = FString::Printf(
		TEXT("VideoPath=%s\nStatus=RECORDING\nStartTime=%s\nDuration=%.1f\nFPS=%d\nResolution=%dx%d\n"),
		*FPaths::ConvertRelativePathToFull(CurrentSessionVideoPath),
		*FDateTime::Now().ToString(),
		CurrentConfig.LastSecondsToRecord,
		CurrentConfig.TargetFPS,
		CurrentConfig.Width,
		CurrentConfig.Height
	);

	if (FFileHelper::SaveStringToFile(MetadataContent, *MetadataPath))
	{
		UE_LOG(LogSentrySdk, Verbose, TEXT("Crash metadata file created: %s"), *MetadataPath);
	}
	else
	{
		UE_LOG(LogSentrySdk, Warning, TEXT("Failed to create crash metadata file: %s"), *MetadataPath);
	}
}

void USentryCrashVideoHandler::RemoveCrashMetadataFile()
{
	FString MetadataPath = GetMetadataFilePath();
	
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (PlatformFile.FileExists(*MetadataPath))
	{
		if (PlatformFile.DeleteFile(*MetadataPath))
		{
			UE_LOG(LogSentrySdk, Verbose, TEXT("Crash metadata file removed: %s"), *MetadataPath);
		}
	}
}

void USentryCrashVideoHandler::RecoverPreviousCrashVideos()
{
	FString CrashVideoDir = GetCrashVideoDirectory();
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();

	if (!PlatformFile.DirectoryExists(*CrashVideoDir))
	{
		return;
	}

	// Find all .meta files (indicates incomplete recordings or buffer dumps)
	TArray<FString> MetadataFiles;
	PlatformFile.FindFilesRecursively(MetadataFiles, *CrashVideoDir, TEXT(".meta"));

	if (MetadataFiles.Num() == 0)
	{
		return;
	}

	UE_LOG(LogSentrySdk, Warning, TEXT("Found %d crash metadata file(s) from previous session(s)."), MetadataFiles.Num());

	for (const FString& MetadataPath : MetadataFiles)
	{
		// Read metadata file
		FString MetadataContent;
		if (!FFileHelper::LoadFileToString(MetadataContent, *MetadataPath))
		{
			UE_LOG(LogSentrySdk, Warning, TEXT("Failed to read metadata file: %s"), *MetadataPath);
			continue;
		}

		// Parse metadata
		FString VideoPath;
		FString CrashVideoPath;
		FString Status;
		TArray<FString> Lines;
		MetadataContent.ParseIntoArrayLines(Lines);
		
		for (const FString& Line : Lines)
		{
			if (Line.StartsWith(TEXT("VideoPath=")))
			{
				VideoPath = Line.RightChop(10); // Remove "VideoPath=" prefix
			}
			else if (Line.StartsWith(TEXT("CrashVideoPath=")))
			{
				CrashVideoPath = Line.RightChop(15); // Remove "CrashVideoPath=" prefix
			}
			else if (Line.StartsWith(TEXT("Status=")))
			{
				Status = Line.RightChop(7); // Remove "Status=" prefix
			}
		}

		// Handle emergency crash recovery videos
		if (Status == TEXT("CRASH_RECORDED"))
		{
			UE_LOG(LogSentrySdk, Log, TEXT("Found crash video metadata from previous session: %s"), *CrashVideoPath);
			
			// EncodeCircularBufferToVideo creates a fully encoded MP4 file with "_crash_recovery.mp4" suffix
			FString CrashRecoveryVideoPath = CrashVideoPath + TEXT("_crash_recovery.mp4");
			
			// Check if the encoded crash recovery video exists
			if (PlatformFile.FileExists(*CrashRecoveryVideoPath))
			{
				int64 FileSize = PlatformFile.FileSize(*CrashRecoveryVideoPath);
				
				if (FileSize > 0)
				{
					UE_LOG(LogSentrySdk, Log, TEXT("Found crash video from previous session: %s (%.2f MB)"), 
						*CrashRecoveryVideoPath, FileSize / (1024.0f * 1024.0f));
					
					// The video was already attached to the crash report by TryCaptureEmergencyCrashVideo()
					// during the crash. We just need to clean up the leftover files.
					// Don't send it again to avoid duplicate uploads.
					UE_LOG(LogSentrySdk, Log, TEXT("Video was already attached to crash report. Cleaning up."));
					
					// Delete the video file (it was already sent with the crash)
					PlatformFile.DeleteFile(*CrashRecoveryVideoPath);
				}
				else
				{
					UE_LOG(LogSentrySdk, Warning, TEXT("Crash recovery video is empty, deleting: %s"), *CrashRecoveryVideoPath);
					PlatformFile.DeleteFile(*CrashRecoveryVideoPath);
				}
			}
			else
			{
				UE_LOG(LogSentrySdk, Verbose, TEXT("Crash recovery video not found (may have been cleaned up already): %s"), *CrashRecoveryVideoPath);
			}
			
			// Clean up metadata file after processing
			PlatformFile.DeleteFile(*MetadataPath);
			continue; // Continue to next metadata file
		}

		// Normal metadata cleanup (Status=RECORDING files)
		// These are created when recording starts and should be deleted when recording stops normally.
		// If they exist on startup, it means the app terminated while recording was active.
		if (VideoPath.IsEmpty())
		{
			UE_LOG(LogSentrySdk, Verbose, TEXT("Cleaning up orphaned metadata file: %s"), *MetadataPath);
			PlatformFile.DeleteFile(*MetadataPath);
			continue;
		}

		// Check if a video file exists (shouldn't happen for Status=RECORDING)
		if (PlatformFile.FileExists(*VideoPath))
		{
			int64 FileSize = PlatformFile.FileSize(*VideoPath);
			
			if (FileSize > 0)
			{
				UE_LOG(LogSentrySdk, Log, TEXT("Found orphaned video file from previous session: %s (%.2f MB)"), 
					*VideoPath, FileSize / (1024.0f * 1024.0f));
				
				// This video file shouldn't exist for Status=RECORDING metadata.
				// It may be from a manual capture or interrupted session.
				// Just clean it up (it was likely already sent or is incomplete).
				UE_LOG(LogSentrySdk, Log, TEXT("Cleaning up orphaned video file."));
				PlatformFile.DeleteFile(*VideoPath);
			}
			else
			{
				UE_LOG(LogSentrySdk, Verbose, TEXT("Deleting empty video file: %s"), *VideoPath);
				PlatformFile.DeleteFile(*VideoPath);
			}
		}

		// Clean up metadata file
		PlatformFile.DeleteFile(*MetadataPath);
	}

	UE_LOG(LogSentrySdk, Log, TEXT("Crash video recovery complete."));
}

