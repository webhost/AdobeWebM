///////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2013, Brendan Bolles
// 
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
// *	   Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
// *	   Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
///////////////////////////////////////////////////////////////////////////

// ------------------------------------------------------------------------
//
// WebM plug-in for Premiere
//
// by Brendan Bolles <brendan@fnordware.com>
//
// ------------------------------------------------------------------------


#include "WebM_Premiere_Import.h"


extern "C" {

#include "vpx/vpx_decoder.h"
#include "vpx/vp8dx.h"

#include <vorbis/codec.h>

}

#include "mkvparser.hpp"

#include <assert.h>


#ifdef PRMAC_ENV
	#include <mach/mach.h>
#endif

int g_num_cpus = 1;



#if IMPORTMOD_VERSION <= IMPORTMOD_VERSION_9
typedef PrSDKPPixCacheSuite2 PrCacheSuite;
#define PrCacheVersion	kPrSDKPPixCacheSuiteVersion2
#else
typedef PrSDKPPixCacheSuite PrCacheSuite;
#define PrCacheVersion	kPrSDKPPixCacheSuiteVersion
#endif


class PrMkvReader : public mkvparser::IMkvReader
{
  public:
	PrMkvReader(imFileRef fileRef) : _fileRef(fileRef) {}
	virtual ~PrMkvReader() {}
	
	virtual int Read(long long pos, long len, unsigned char* buf);
	virtual int Length(long long* total, long long* available);
	
	const imFileRef FileRef() const { return _fileRef; }
	
	enum {
		PrMkvError = -1,
		PrMkvSuccess = 0
	};
	
  private:
	const imFileRef _fileRef;
};


int PrMkvReader::Read(long long pos, long len, unsigned char* buf)
{
#ifdef PRWIN_ENV
	LARGE_INTEGER lpos, out;

	lpos.QuadPart = pos;

	BOOL result = SetFilePointerEx(_fileRef, lpos, &out, FILE_BEGIN);
	
	DWORD count = len, out2;
	
	result = ReadFile(_fileRef, (LPVOID)buf, count, &out2, NULL);

	return (result && len == out2) ? PrMkvSuccess : PrMkvError;
#else
	ByteCount count = len, out = 0;
	
	OSErr result = FSReadFork(CAST_REFNUM(_fileRef), fsFromStart, pos, count, buf, &out);

	return (result == noErr && len == out) ? PrMkvSuccess : PrMkvError;
#endif
}


int PrMkvReader::Length(long long* total, long long* available)
{
#ifdef PRWIN_ENV
	LARGE_INTEGER len;

	BOOL ok = GetFileSizeEx(_fileRef, &len);
	
	if(ok)
	{
		if(total)
			*total = len.QuadPart;

		if(available)
			*available = len.QuadPart;

		return PrMkvSuccess;
	}
	else
		return PrMkvError;
#else
	SInt64 fork_size = 0;
	
	OSErr result = FSGetForkSize(CAST_REFNUM(_fileRef), &fork_size);
		
	if(result == noErr)
	{
		if(total)
			*total = fork_size;
		
		if(available)
			*available = fork_size;
		
		return PrMkvSuccess;
	}
	else
		return PrMkvError;
#endif
}


typedef struct
{	
	csSDK_int32				importerID;
	csSDK_int32				fileType;
	csSDK_int32				width;
	csSDK_int32				height;
	csSDK_int32				frameRateNum;
	csSDK_int32				frameRateDen;
	float					audioSampleRate;
	int						numChannels;
	
	PrMkvReader				*reader;
	mkvparser::Segment		*segment;
	int						video_track;
	int						audio_track;
	unsigned int			time_mult;
	
	PlugMemoryFuncsPtr		memFuncs;
	SPBasicSuite			*BasicSuite;
	PrSDKPPixCreatorSuite	*PPixCreatorSuite;
	PrCacheSuite			*PPixCacheSuite;
	PrSDKPPixSuite			*PPixSuite;
	PrSDKPPix2Suite			*PPix2Suite;
	PrSDKTimeSuite			*TimeSuite;
	PrSDKImporterFileManagerSuite *FileSuite;
} ImporterLocalRec8, *ImporterLocalRec8Ptr, **ImporterLocalRec8H;


static prMALError 
SDKInit(
	imStdParms		*stdParms, 
	imImportInfoRec	*importInfo)
{
	PrSDKAppInfoSuite *appInfoSuite = NULL;
	stdParms->piSuites->utilFuncs->getSPBasicSuite()->AcquireSuite(kPrSDKAppInfoSuite, kPrSDKAppInfoSuiteVersion, (const void**)&appInfoSuite);
	
	if(appInfoSuite)
	{
		int fourCC = 0;
	
		appInfoSuite->GetAppInfo(PrSDKAppInfoSuite::kAppInfo_AppFourCC, (void *)&fourCC);
	
		stdParms->piSuites->utilFuncs->getSPBasicSuite()->ReleaseSuite(kPrSDKAppInfoSuite, kPrSDKAppInfoSuiteVersion);
		
		// return this error if you don't want to run in AE
		//if(fourCC == kAppAfterEffects)
		//	return imOtherErr;
	}
	
	importInfo->setupOnDblClk		= kPrFalse;		// If user dbl-clicks file you imported, pop your setup dialog
	importInfo->canSave				= kPrFalse;		// Can 'save as' files to disk, real file only
	
	importInfo->canDelete			= kPrFalse;		// File importers only, use if you only if you have child files
	importInfo->dontCache			= kPrFalse;		// Don't let Premiere cache these files
	importInfo->hasSetup			= kPrFalse;		// Set to kPrTrue if you have a setup dialog
	importInfo->keepLoaded			= kPrFalse;		// If you MUST stay loaded use, otherwise don't: play nice
	importInfo->priority			= 0;
	importInfo->canTrim				= kPrFalse;
	importInfo->canCalcSizes		= kPrFalse;
	if(stdParms->imInterfaceVer >= IMPORTMOD_VERSION_6)
	{
		importInfo->avoidAudioConform = kPrTrue;
	}							

#ifdef PRMAC_ENV
	// get number of CPUs using Mach calls
	host_basic_info_data_t hostInfo;
	mach_msg_type_number_t infoCount;
	
	infoCount = HOST_BASIC_INFO_COUNT;
	host_info(mach_host_self(), HOST_BASIC_INFO, 
			  (host_info_t)&hostInfo, &infoCount);
	
	g_num_cpus = hostInfo.avail_cpus;
#else // WIN_ENV
	SYSTEM_INFO systemInfo;
	GetSystemInfo(&systemInfo);

	g_num_cpus = systemInfo.dwNumberOfProcessors;
#endif

	return malNoError;
}

static prMALError 
SDKGetIndFormat(
	imStdParms		*stdParms, 
	csSDK_size_t	index, 
	imIndFormatRec	*SDKIndFormatRec)
{
	prMALError	result		= malNoError;
	char formatname[255]	= "WebM Format";
	char shortname[32]		= "WebM";
	char platformXten[256]	= "webm\0\0";

	switch(index)
	{
		//	Add a case for each filetype.
		
		case 0:		
			SDKIndFormatRec->filetype			= 'WebM';

			SDKIndFormatRec->canWriteTimecode	= kPrFalse;
			SDKIndFormatRec->canWriteMetaData	= kPrFalse;

			SDKIndFormatRec->flags = xfCanImport;

			#ifdef PRWIN_ENV
			strcpy_s(SDKIndFormatRec->FormatName, sizeof (SDKIndFormatRec->FormatName), formatname);				// The long name of the importer
			strcpy_s(SDKIndFormatRec->FormatShortName, sizeof (SDKIndFormatRec->FormatShortName), shortname);		// The short (menu name) of the importer
			strcpy_s(SDKIndFormatRec->PlatformExtension, sizeof (SDKIndFormatRec->PlatformExtension), platformXten);	// The 3 letter extension
			#else
			strcpy(SDKIndFormatRec->FormatName, formatname);			// The Long name of the importer
			strcpy(SDKIndFormatRec->FormatShortName, shortname);		// The short (menu name) of the importer
			strcpy(SDKIndFormatRec->PlatformExtension, platformXten);	// The 3 letter extension
			#endif

			break;

		default:
			result = imBadFormatIndex;
	}

	return result;
}


prMALError 
SDKOpenFile8(
	imStdParms		*stdParms, 
	imFileRef		*SDKfileRef, 
	imFileOpenRec8	*SDKfileOpenRec8)
{
	prMALError			result = malNoError;

	ImporterLocalRec8H	localRecH = NULL;
	ImporterLocalRec8Ptr localRecP = NULL;

	// Private data stores:
	// 1. Pointers to suites
	// 2. Width, height, and timing information
	// 3. File path

	if(SDKfileOpenRec8->privatedata)
	{
		localRecH = (ImporterLocalRec8H)SDKfileOpenRec8->privatedata;

		stdParms->piSuites->memFuncs->lockHandle(reinterpret_cast<char**>(localRecH));

		localRecP = reinterpret_cast<ImporterLocalRec8Ptr>( *localRecH );
	}
	else
	{
		localRecH = (ImporterLocalRec8H)stdParms->piSuites->memFuncs->newHandle(sizeof(ImporterLocalRec8));
		SDKfileOpenRec8->privatedata = (PrivateDataPtr)localRecH;

		stdParms->piSuites->memFuncs->lockHandle(reinterpret_cast<char**>(localRecH));

		localRecP = reinterpret_cast<ImporterLocalRec8Ptr>( *localRecH );
		
		localRecP->reader = NULL;
		localRecP->segment = NULL;
		localRecP->video_track = -1;
		localRecP->audio_track = -1;
		localRecP->time_mult = 1;
		
		// Acquire needed suites
		localRecP->memFuncs = stdParms->piSuites->memFuncs;
		localRecP->BasicSuite = stdParms->piSuites->utilFuncs->getSPBasicSuite();
		if(localRecP->BasicSuite)
		{
			localRecP->BasicSuite->AcquireSuite(kPrSDKPPixCreatorSuite, kPrSDKPPixCreatorSuiteVersion, (const void**)&localRecP->PPixCreatorSuite);
			localRecP->BasicSuite->AcquireSuite(kPrSDKPPixCacheSuite, PrCacheVersion, (const void**)&localRecP->PPixCacheSuite);
			localRecP->BasicSuite->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, (const void**)&localRecP->PPixSuite);
			localRecP->BasicSuite->AcquireSuite(kPrSDKPPix2Suite, kPrSDKPPix2SuiteVersion, (const void**)&localRecP->PPix2Suite);
			localRecP->BasicSuite->AcquireSuite(kPrSDKTimeSuite, kPrSDKTimeSuiteVersion, (const void**)&localRecP->TimeSuite);
			localRecP->BasicSuite->AcquireSuite(kPrSDKImporterFileManagerSuite, kPrSDKImporterFileManagerSuiteVersion, (const void**)&localRecP->FileSuite);
		}

		localRecP->importerID = SDKfileOpenRec8->inImporterID;
		localRecP->fileType = SDKfileOpenRec8->fileinfo.filetype;
	}


	SDKfileOpenRec8->fileinfo.fileref = *SDKfileRef = reinterpret_cast<imFileRef>(imInvalidHandleValue);


	if(localRecP)
	{
		const prUTF16Char *path = SDKfileOpenRec8->fileinfo.filepath;
	
	#ifdef PRWIN_ENV
		HANDLE fileH = CreateFileW(path,
									GENERIC_READ,
									FILE_SHARE_READ,
									NULL,
									OPEN_EXISTING,
									FILE_ATTRIBUTE_NORMAL,
									NULL);
									
		if(fileH != imInvalidHandleValue)
		{
			SDKfileOpenRec8->fileinfo.fileref = *SDKfileRef = fileH;
		}
		else
			result = imFileOpenFailed;
	#else
		FSIORefNum refNum = CAST_REFNUM(imInvalidHandleValue);
				
		CFStringRef filePathCFSR = CFStringCreateWithCharacters(NULL, path, prUTF16CharLength(path));
													
		CFURLRef filePathURL = CFURLCreateWithFileSystemPath(NULL, filePathCFSR, kCFURLPOSIXPathStyle, false);
		
		if(filePathURL != NULL)
		{
			FSRef fileRef;
			Boolean success = CFURLGetFSRef(filePathURL, &fileRef);
			
			if(success)
			{
				HFSUniStr255 dataForkName;
				FSGetDataForkName(&dataForkName);
			
				OSErr err = FSOpenFork(	&fileRef,
										dataForkName.length,
										dataForkName.unicode,
										fsRdWrPerm,
										&refNum);
			}
										
			CFRelease(filePathURL);
		}
									
		CFRelease(filePathCFSR);

		if(CAST_FILEREF(refNum) != imInvalidHandleValue)
		{
			SDKfileOpenRec8->fileinfo.fileref = *SDKfileRef = CAST_FILEREF(refNum);
		}
		else
			result = imFileOpenFailed;
	#endif

	}

	if(result == malNoError && localRecP->reader == NULL)
	{
		assert(localRecP->segment == NULL);
	
		localRecP->reader = new PrMkvReader(*SDKfileRef);
		
		long long pos = 0;

		mkvparser::EBMLHeader ebmlHeader;

		ebmlHeader.Parse(localRecP->reader, pos);
		
		long long ret = mkvparser::Segment::CreateInstance(localRecP->reader, pos, localRecP->segment);
		
		if(ret >= 0 && localRecP->segment != NULL)
		{
			ret = localRecP->segment->Load();
			
			if(ret >= 0)
			{
				const mkvparser::Tracks* pTracks = localRecP->segment->GetTracks();
				
				for(int t=0; t < pTracks->GetTracksCount(); t++)
				{
					const mkvparser::Track* const pTrack = pTracks->GetTrackByIndex(t);
					
					if(pTrack != NULL)
					{
						const long trackType = pTrack->GetType();
						const long trackNumber = pTrack->GetNumber();
						
						if(trackType == mkvparser::Track::kVideo)
						{
							const mkvparser::VideoTrack* const pVideoTrack = static_cast<const mkvparser::VideoTrack*>(pTrack);
							
							if(pVideoTrack)
							{
								localRecP->video_track = trackNumber;
							}
						}
						else if(trackType == mkvparser::Track::kAudio)
						{
							const mkvparser::AudioTrack* const pAudioTrack = static_cast<const mkvparser::AudioTrack*>(pTrack);
							
							if(pAudioTrack)
							{
								localRecP->audio_track = trackNumber;
							}
						}
 					}
				}
				
				if(localRecP->video_track == -1 && localRecP->audio_track == -1)
				{
					result = imFileHasNoImportableStreams;
				}
			}
		}
		else
			result = imBadHeader;
	}
	
	// close file and delete private data if we got a bad file
	if(result != malNoError)
	{
		if(SDKfileOpenRec8->privatedata)
		{
			stdParms->piSuites->memFuncs->disposeHandle(reinterpret_cast<PrMemoryHandle>(SDKfileOpenRec8->privatedata));
			SDKfileOpenRec8->privatedata = NULL;
		}
	}
	else
	{
		stdParms->piSuites->memFuncs->unlockHandle(reinterpret_cast<char**>(SDKfileOpenRec8->privatedata));
	}

	return result;
}


//-------------------------------------------------------------------
//	"Quiet" the file (it's being closed, but you maintain your Private data).
//  Premiere does this when you put the app in the background so it's not
//  sitting there with a bunch of open (locked) files.
//	
//	NOTE:	If you don't set any privateData, you will not get an imCloseFile call
//			so close it up here.

static prMALError 
SDKQuietFile(
	imStdParms			*stdParms, 
	imFileRef			*SDKfileRef, 
	void				*privateData)
{
	// If file has not yet been closed
	if (SDKfileRef && *SDKfileRef != imInvalidHandleValue)
	{
		ImporterLocalRec8H ldataH	= reinterpret_cast<ImporterLocalRec8H>(privateData);

		stdParms->piSuites->memFuncs->lockHandle(reinterpret_cast<char**>(ldataH));

		ImporterLocalRec8Ptr localRecP = reinterpret_cast<ImporterLocalRec8Ptr>( *ldataH );

		if(localRecP->segment)
		{
			delete localRecP->segment;
			
			localRecP->segment = NULL;
		}
		
		if(localRecP->reader)
		{
			delete localRecP->reader;
			
			localRecP->reader = NULL;
		}

		stdParms->piSuites->memFuncs->unlockHandle(reinterpret_cast<char**>(ldataH));

	#ifdef PRWIN_ENV
		CloseHandle(*SDKfileRef);
	#else
		FSCloseFork( CAST_REFNUM(*SDKfileRef) );
	#endif
	
		*SDKfileRef = imInvalidHandleValue;
	}

	return malNoError; 
}


//-------------------------------------------------------------------
//	Close the file.  You MUST have allocated Private data in imGetPrefs or you will not
//	receive this call.

static prMALError 
SDKCloseFile(
	imStdParms			*stdParms, 
	imFileRef			*SDKfileRef,
	void				*privateData) 
{
	ImporterLocalRec8H ldataH	= reinterpret_cast<ImporterLocalRec8H>(privateData);
	
	// If file has not yet been closed
	if(SDKfileRef && *SDKfileRef != imInvalidHandleValue)
	{
		SDKQuietFile(stdParms, SDKfileRef, privateData);
	}

	// Remove the privateData handle.
	// CLEANUP - Destroy the handle we created to avoid memory leaks
	if(ldataH && *ldataH && (*ldataH)->BasicSuite)
	{
		stdParms->piSuites->memFuncs->lockHandle(reinterpret_cast<char**>(ldataH));

		ImporterLocalRec8Ptr localRecP = reinterpret_cast<ImporterLocalRec8Ptr>( *ldataH );;

		localRecP->BasicSuite->ReleaseSuite(kPrSDKPPixCreatorSuite, kPrSDKPPixCreatorSuiteVersion);
		localRecP->BasicSuite->ReleaseSuite(kPrSDKPPixCacheSuite, PrCacheVersion);
		localRecP->BasicSuite->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
		localRecP->BasicSuite->ReleaseSuite(kPrSDKPPix2Suite, kPrSDKPPix2SuiteVersion);
		localRecP->BasicSuite->ReleaseSuite(kPrSDKTimeSuite, kPrSDKTimeSuiteVersion);
		localRecP->BasicSuite->ReleaseSuite(kPrSDKImporterFileManagerSuite, kPrSDKImporterFileManagerSuiteVersion);

		stdParms->piSuites->memFuncs->disposeHandle(reinterpret_cast<PrMemoryHandle>(ldataH));
	}

	return malNoError;
}


// Go ahead and overwrite any existing file. Premiere will have already checked and warned the user if file will be overwritten.
// Of course, if there are child files, you should check and return imSaveErr if appropriate.
//
// I'm not actually sure if this will ever get called.  It was in the Premiere sample so
// I just left it.  All the calls are not format specific.
static prMALError 
SDKSaveFile8(
	imStdParms			*stdParms, 
	imSaveFileRec8		*SDKSaveFileRec8) 
{
	prMALError	result = malNoError;
	#ifdef PRMAC_ENV
	CFStringRef			sourceFilePathCFSR,
						destFilePathCFSR,
						destFolderCFSR,
						destFileNameCFSR;
	CFRange				destFileNameRange,
						destFolderRange;
	CFURLRef			sourceFilePathURL,
						destFolderURL;
	FSRef				sourceFileRef,
						destFolderRef;
												
	// Convert prUTF16Char filePaths to FSRefs for paths
	sourceFilePathCFSR = CFStringCreateWithCharacters(	kCFAllocatorDefault,
														SDKSaveFileRec8->sourcePath,
														prUTF16CharLength(SDKSaveFileRec8->sourcePath));
	destFilePathCFSR = CFStringCreateWithCharacters(	kCFAllocatorDefault,
														SDKSaveFileRec8->destPath,
														prUTF16CharLength(SDKSaveFileRec8->destPath));
														
	// Separate the folder path from the file name
	destFileNameRange = CFStringFind(	destFilePathCFSR,
										CFSTR("/"),
										kCFCompareBackwards);
	destFolderRange.location = 0;
	destFolderRange.length = destFileNameRange.location;
	destFileNameRange.location += destFileNameRange.length;
	destFileNameRange.length = CFStringGetLength(destFilePathCFSR) - destFileNameRange.location;
	destFolderCFSR = CFStringCreateWithSubstring(	kCFAllocatorDefault,
													destFilePathCFSR,
													destFolderRange);
	destFileNameCFSR = CFStringCreateWithSubstring(	kCFAllocatorDefault,
													destFilePathCFSR,
													destFileNameRange);
		
	// Make FSRefs
	sourceFilePathURL = CFURLCreateWithFileSystemPath(	kCFAllocatorDefault,
														sourceFilePathCFSR,
														kCFURLPOSIXPathStyle,
														false);
	destFolderURL = CFURLCreateWithFileSystemPath(	kCFAllocatorDefault,
													destFolderCFSR,
													kCFURLPOSIXPathStyle,
													true);
	CFURLGetFSRef(sourceFilePathURL, &sourceFileRef);
	CFURLGetFSRef(destFolderURL, &destFolderRef);						
	#endif
	
	if (SDKSaveFileRec8->move)
	{
		#ifdef PRWIN_ENV
		if( MoveFileW(SDKSaveFileRec8->sourcePath, SDKSaveFileRec8->destPath) == 0)
		{
			result = imSaveErr;
		}
		#else
		if( FSCopyObjectSync(	&sourceFileRef,
								&destFolderRef,
								destFileNameCFSR,
								NULL,
								kFSFileOperationOverwrite))
		{
			result = imSaveErr;
		}
		#endif
	}
	else
	{
		#ifdef PRWIN_ENV
		if( CopyFileW (SDKSaveFileRec8->sourcePath, SDKSaveFileRec8->destPath, kPrTrue) == 0)
		{
			result = imSaveErr;
		}
		#else
		if ( FSMoveObjectSync(	&sourceFileRef,
								&destFolderRef,
								destFileNameCFSR,
								NULL,
								kFSFileOperationOverwrite))
		{
			result = imSaveErr;
		}
		#endif
	}
	return result;
}


// This was also in the SDK sample, so figured I'd just leave it here.
static prMALError 
SDKDeleteFile8(
	imStdParms			*stdParms, 
	imDeleteFileRec8	*SDKDeleteFileRec8)
{
	prMALError	result = malNoError;

	#ifdef PRWIN_ENV
	if( DeleteFileW(SDKDeleteFileRec8->deleteFilePath) )
	{
		result = imDeleteErr;
	}
	#else
	CFStringRef	filePathCFSR;
	CFURLRef	filePathURL;
	FSRef		fileRef;

	filePathCFSR = CFStringCreateWithCharacters(kCFAllocatorDefault,
												SDKDeleteFileRec8->deleteFilePath,
												prUTF16CharLength(SDKDeleteFileRec8->deleteFilePath));
	filePathURL = CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
												filePathCFSR,
												kCFURLPOSIXPathStyle,
												false);
	CFURLGetFSRef(filePathURL, &fileRef);					
	if( FSDeleteObject(&fileRef) )
	{
		result = imDeleteErr;
	}
	#endif
	
	return result;
}



static prMALError 
SDKGetIndPixelFormat(
	imStdParms			*stdParms,
	csSDK_size_t		idx,
	imIndPixelFormatRec	*SDKIndPixelFormatRec) 
{
	prMALError	result	= malNoError;
	//ImporterLocalRec8H	ldataH	= reinterpret_cast<ImporterLocalRec8H>(SDKIndPixelFormatRec->privatedata);

	switch(idx)
	{
		// just support one pixel format, 8-bit BGRA
		case 0:
			SDKIndPixelFormatRec->outPixelFormat = PrPixelFormat_YUV_420_MPEG2_FRAME_PICTURE_PLANAR_8u_709;
			break;
	
		default:
			result = imBadFormatIndex;
			break;
	}

	return result;	
}


// File analysis - Supplies supplemental compression information to File Properties dialog
// I'm just using this opportunity for inform the user that they can double-click.
static prMALError 
SDKAnalysis(
	imStdParms		*stdParms,
	imFileRef		SDKfileRef,
	imAnalysisRec	*SDKAnalysisRec)
{
	// if you wanted to get the private data
	//ImporterLocalRec8H ldataH = reinterpret_cast<ImporterLocalRec8H>(SDKAnalysisRec->privatedata);

	const char *properties_messsage = "WebM info goes here";

	if(SDKAnalysisRec->buffersize > strlen(properties_messsage))
		strcpy(SDKAnalysisRec->buffer, properties_messsage);

	return malNoError;
}


static int
webm_guess_framerate(mkvparser::Segment *segment,
						int				video_track,
						unsigned int	*fps_den,
						unsigned int	*fps_num)
{
	unsigned int i = 0;
	uint64_t     tstamp = 0;

	const mkvparser::Cluster* pCluster = segment->GetFirst();
	const mkvparser::Tracks* pTracks = segment->GetTracks();

	long status = 0;

	while( (pCluster != NULL) && !pCluster->EOS() && status >= 0 && tstamp < 1000000000 && i < 50)
	{
		const mkvparser::BlockEntry* pBlockEntry = NULL;
		
		status = pCluster->GetFirst(pBlockEntry);
		
		while( (pBlockEntry != NULL) && !pBlockEntry->EOS() && status >= 0 && tstamp < 1000000000 && i < 50)
		{
			const mkvparser::Block* const pBlock  = pBlockEntry->GetBlock();
			const long long trackNum = pBlock->GetTrackNumber();
			
			if(trackNum == video_track)
			{
				const unsigned long tn = static_cast<unsigned long>(trackNum);
				const mkvparser::Track* const pTrack = pTracks->GetTrackByNumber(tn);
				
				if(pTrack)
				{
					assert(pTrack->GetType() == mkvparser::Track::kVideo);
					assert(pBlock->GetFrameCount() == 1);
					
					tstamp = pBlock->GetTime(pCluster);
					
					i++;
				}
			}
			
			status = pCluster->GetNext(pBlockEntry, pBlockEntry);
		}
		
		pCluster = segment->GetNext(pCluster);
	}


	*fps_num = (i - 1) * 1000000;
	*fps_den = (unsigned int)(tstamp / 1000);

	return 0;
}



//-------------------------------------------------------------------
// Populate the imFileInfoRec8 structure describing this file instance
// to Premiere.  Check file validity, allocate any private instance data 
// to share between different calls.
//
// Actually, I'm currently verifying the file back during the open phase.
// Is that a problem?  Doing it that way because some FFmpeg structures
// are associated with the file reading operations but we have to know
// if it's actually a PNG first.

prMALError 
SDKGetInfo8(
	imStdParms			*stdParms, 
	imFileAccessRec8	*fileAccessInfo8, 
	imFileInfoRec8		*SDKFileInfo8)
{
	prMALError					result				= malNoError;


	SDKFileInfo8->vidInfo.supportsAsyncIO			= kPrFalse;
	SDKFileInfo8->vidInfo.supportsGetSourceVideo	= kPrTrue;
	SDKFileInfo8->vidInfo.hasPulldown				= kPrFalse;
	SDKFileInfo8->hasDataRate						= kPrFalse;


	// private data
	assert(SDKFileInfo8->privatedata);
	ImporterLocalRec8H ldataH = reinterpret_cast<ImporterLocalRec8H>(SDKFileInfo8->privatedata);
	stdParms->piSuites->memFuncs->lockHandle(reinterpret_cast<char**>(ldataH));
	ImporterLocalRec8Ptr localRecP = reinterpret_cast<ImporterLocalRec8Ptr>( *ldataH );


	SDKFileInfo8->hasVideo = kPrFalse;
	SDKFileInfo8->hasAudio = kPrFalse;
	
	
	if(localRecP)
	{
		assert(localRecP->segment != NULL);
		
		if(localRecP->segment != NULL)
		{
			const mkvparser::SegmentInfo* const pSegmentInfo = localRecP->segment->GetInfo();
			
			const long long duration = pSegmentInfo->GetDuration();
			
			const mkvparser::Tracks* pTracks = localRecP->segment->GetTracks();
			
			if(localRecP->video_track >= 0)
			{
				const mkvparser::Track* const pTrack = pTracks->GetTrackByNumber(localRecP->video_track);
				
				if(pTrack != NULL)
				{
					const long trackType = pTrack->GetType();
				
					if(trackType == mkvparser::Track::kVideo)
					{
						const mkvparser::VideoTrack* const pVideoTrack = static_cast<const mkvparser::VideoTrack*>(pTrack);
						
						if(pVideoTrack)
						{
							const double rate = pVideoTrack->GetFrameRate();
							
							unsigned int fps_num = 0;
							unsigned int fps_den = 0;
							
							if(rate != 0)
							{
								fps_den = 1000;
								fps_num = rate * fps_den;
							}
							
							webm_guess_framerate(localRecP->segment, localRecP->video_track, &fps_den, &fps_num);
							
							// getting some very large fps numbers, need to lower them
							if(fps_num % 1000 == 0 && fps_den % 1000 == 0)
							{
								localRecP->time_mult = 1000;
								
								fps_num /= localRecP->time_mult;
								fps_den /= localRecP->time_mult;
							}
							else
								localRecP->time_mult = 1;
							
							int frames = (duration * fps_num / fps_den) / 1000000000UL;
														
							// Video information
							SDKFileInfo8->hasVideo				= kPrTrue;
							SDKFileInfo8->vidInfo.subType		= PrPixelFormat_YUV_420_MPEG2_FRAME_PICTURE_PLANAR_8u_709;
							SDKFileInfo8->vidInfo.imageWidth	= pVideoTrack->GetWidth();
							SDKFileInfo8->vidInfo.imageHeight	= pVideoTrack->GetHeight();
							SDKFileInfo8->vidInfo.depth			= 24;	// The bit depth of the video
							SDKFileInfo8->vidInfo.fieldType		= prFieldsNone; // or prFieldsUnknown
							SDKFileInfo8->vidInfo.isStill		= kPrFalse;
							SDKFileInfo8->vidInfo.noDuration	= imNoDurationFalse;
							SDKFileInfo8->vidDuration			= frames * fps_den;
							SDKFileInfo8->vidScale				= fps_num;
							SDKFileInfo8->vidSampleSize			= fps_den;

							SDKFileInfo8->vidInfo.alphaType	= alphaNone;

							SDKFileInfo8->vidInfo.pixelAspectNum = 1;
							SDKFileInfo8->vidInfo.pixelAspectDen = 1;

							// store some values we want to get without going to the file
							localRecP->width = SDKFileInfo8->vidInfo.imageWidth;
							localRecP->height = SDKFileInfo8->vidInfo.imageHeight;

							localRecP->frameRateNum = SDKFileInfo8->vidScale;
							localRecP->frameRateDen = SDKFileInfo8->vidSampleSize;
						}
					}
				}
			}
			
			if(localRecP->audio_track >= 0)
			{
				const mkvparser::Track* const pTrack = pTracks->GetTrackByNumber(localRecP->audio_track);
				
				if(pTrack != NULL)
				{
					const long trackType = pTrack->GetType();
					
					if(trackType == mkvparser::Track::kAudio)
					{
						const mkvparser::AudioTrack* const pAudioTrack = static_cast<const mkvparser::AudioTrack*>(pTrack);
						
						if(pAudioTrack)
						{
							const long long bitDepth = pAudioTrack->GetBitDepth();
							
							
							// Audio information
							SDKFileInfo8->hasAudio				= kPrTrue;
							SDKFileInfo8->audInfo.numChannels	= pAudioTrack->GetChannels();
							SDKFileInfo8->audInfo.sampleRate	= pAudioTrack->GetSamplingRate();
							SDKFileInfo8->audInfo.sampleType	= bitDepth == 8 ? kPrAudioSampleType_8BitInt :
																	bitDepth == 16 ? kPrAudioSampleType_16BitInt :
																	bitDepth == 24 ? kPrAudioSampleType_24BitInt :
																	bitDepth == 32 ? kPrAudioSampleType_32BitFloat :
																	bitDepth == 64 ? kPrAudioSampleType_64BitFloat :
																	kPrAudioSampleType_Compressed;
																	
							SDKFileInfo8->audDuration			= (uint64_t)SDKFileInfo8->audInfo.sampleRate * duration / 1000000000UL;
							
							
							localRecP->audioSampleRate			= SDKFileInfo8->audInfo.sampleRate;
							localRecP->numChannels				= SDKFileInfo8->audInfo.numChannels;
						}
					}
				}
			}
		}
	}
		
	stdParms->piSuites->memFuncs->unlockHandle(reinterpret_cast<char**>(ldataH));

	return result;
}


static prMALError 
SDKCalcSize8(
	imStdParms			*stdParms, 
	imCalcSizeRec		*calcSizeRec,
	imFileAccessRec8	*fileAccessRec8)
{
	// tell Premiere the file size
	
	return imUnsupported;
}


static prMALError 
SDKPreferredFrameSize(
	imStdParms					*stdparms, 
	imPreferredFrameSizeRec		*preferredFrameSizeRec)
{
	prMALError			result	= imIterateFrameSizes;
	ImporterLocalRec8H	ldataH	= reinterpret_cast<ImporterLocalRec8H>(preferredFrameSizeRec->inPrivateData);

	stdparms->piSuites->memFuncs->lockHandle(reinterpret_cast<char**>(ldataH));

	ImporterLocalRec8Ptr localRecP = reinterpret_cast<ImporterLocalRec8Ptr>( *ldataH );


	bool can_shrink = false; // doesn't look like we can decode a smaller frame

	if(preferredFrameSizeRec->inIndex == 0)
	{
		preferredFrameSizeRec->outWidth = localRecP->width;
		preferredFrameSizeRec->outHeight = localRecP->height;
	}
	else
	{
		// we store width and height in private data so we can produce it here
		const int divisor = 1; //pow(2, preferredFrameSizeRec->inIndex);
		
		if(can_shrink &&
			preferredFrameSizeRec->inIndex < 4 &&
			localRecP->width % divisor == 0 &&
			localRecP->height % divisor == 0 )
		{
			preferredFrameSizeRec->outWidth = localRecP->width / divisor;
			preferredFrameSizeRec->outHeight = localRecP->height / divisor;
		}
		else
			result = malNoError;
	}


	stdparms->piSuites->memFuncs->unlockHandle(reinterpret_cast<char**>(ldataH));

	return result;
}


static prMALError 
SDKGetSourceVideo(
	imStdParms			*stdParms, 
	imFileRef			fileRef, 
	imSourceVideoRec	*sourceVideoRec)
{
	prMALError		result		= malNoError;
	csSDK_int32		theFrame	= 0;
	imFrameFormat	*frameFormat;

	// privateData
	ImporterLocalRec8H ldataH = reinterpret_cast<ImporterLocalRec8H>(sourceVideoRec->inPrivateData);
	stdParms->piSuites->memFuncs->lockHandle(reinterpret_cast<char**>(ldataH));
	ImporterLocalRec8Ptr localRecP = reinterpret_cast<ImporterLocalRec8Ptr>( *ldataH );


	PrTime ticksPerSecond = 0;
	localRecP->TimeSuite->GetTicksPerSecond(&ticksPerSecond);
	

	if(localRecP->frameRateDen == 0) // i.e. still frame
	{
		theFrame = 0;
	}
	else
	{
		PrTime ticksPerFrame = (ticksPerSecond * (PrTime)localRecP->frameRateDen) / (PrTime)localRecP->frameRateNum;
		theFrame = sourceVideoRec->inFrameTime / ticksPerFrame;
	}

	// Check to see if frame is already in cache
	result = localRecP->PPixCacheSuite->GetFrameFromCache(	localRecP->importerID,
															0,
															theFrame,
															1,
															sourceVideoRec->inFrameFormats,
															sourceVideoRec->outFrame,
															NULL,
															NULL);

	// If frame is not in the cache, read the frame and put it in the cache; otherwise, we're done
	if(result != suiteError_NoError)
	{
		// ok, we'll read the file - clear error
		result = malNoError;
		
		// get the Premiere buffer
		frameFormat = &sourceVideoRec->inFrameFormats[0];
		prRect theRect;
		if(frameFormat->inFrameWidth == 0 && frameFormat->inFrameHeight == 0)
		{
			frameFormat->inFrameWidth = localRecP->width;
			frameFormat->inFrameHeight = localRecP->height;
		}
		
		// Windows and MacOS have different definitions of Rects, so use the cross-platform prSetRect
		prSetRect(&theRect, 0, 0, frameFormat->inFrameWidth, frameFormat->inFrameHeight);
		

		assert(localRecP->reader != NULL && localRecP->reader->FileRef() == fileRef);
		assert(localRecP->segment != NULL);
		
		if(localRecP->segment)
		{
			const uint64_t fps_num = localRecP->frameRateNum * localRecP->time_mult;
			const uint64_t fps_den = localRecP->frameRateDen * localRecP->time_mult;
			
			uint64_t tstamp = ((uint64_t)theFrame * fps_den * 1000000000UL / fps_num);
			uint64_t tstamp2 = (uint64_t)sourceVideoRec->inFrameTime * 1000UL / ((uint64_t)ticksPerSecond / 1000000UL); // alternate way of calculating it
			
			assert(tstamp == tstamp2);
			
			const uint64_t half_frame_time = (1000000000UL * fps_den / fps_num) / 2; // half-a-frame
			
			if(localRecP->video_track >= 0)
			{
				const mkvparser::Tracks* pTracks = localRecP->segment->GetTracks();
			
				const mkvparser::Track* const pTrack = pTracks->GetTrackByNumber(localRecP->video_track);
				
				if(pTrack != NULL)
				{
					const long trackType = pTrack->GetType();
				
					if(trackType == mkvparser::Track::kVideo)
					{
						const mkvparser::VideoTrack* const pVideoTrack = static_cast<const mkvparser::VideoTrack*>(pTrack);
						
						if(pVideoTrack)
						{
							const mkvparser::BlockEntry* pSeekBlockEntry = NULL;
							
							pVideoTrack->Seek(tstamp, pSeekBlockEntry);
							
							if(pSeekBlockEntry != NULL)
							{
								const char* codec_id = pTrack->GetCodecId();
							
								const vpx_codec_iface_t *iface = (codec_id == std::string("V_VP8") ? vpx_codec_vp8_dx() :
																	codec_id == std::string("V_VP9") ? vpx_codec_vp9_dx() :
																	NULL);
								
								vpx_codec_err_t codec_err = VPX_CODEC_OK;
								
								vpx_codec_ctx_t decoder;
								
								if(iface != NULL)
								{
									vpx_codec_dec_cfg_t config;
									config.threads = g_num_cpus;
									config.w = frameFormat->inFrameWidth;
									config.h = frameFormat->inFrameHeight;
									
									codec_err = vpx_codec_dec_init(&decoder, iface, &config, 0);
								}
								else
									codec_err = VPX_CODEC_ERROR;
								
								
								if(codec_err == VPX_CODEC_OK)
								{
									const mkvparser::Cluster *pCluster = pSeekBlockEntry->GetCluster();
									
									bool first_frame = true;
									bool got_frame = false;
									bool reached_next_iframe = false;
									
									while((pCluster != NULL) && !pCluster->EOS() && !reached_next_iframe && result == malNoError)
									{
										const mkvparser::BlockEntry* pBlockEntry = NULL;
										
										pCluster->GetFirst(pBlockEntry);
										
										while((pBlockEntry != NULL) && !pBlockEntry->EOS() && !reached_next_iframe && result == malNoError)
										{
											const mkvparser::Block *pBlock = pBlockEntry->GetBlock();
										
											if(pBlock->GetTrackNumber() == localRecP->video_track)
											{
												assert(pBlock->GetFrameCount() == 1);
												
												long long packet_tstamp = pBlock->GetTime(pCluster);
												
												const mkvparser::Block::Frame& blockFrame = pBlock->GetFrame(0);
												
												unsigned int length = blockFrame.len;
												uint8_t *data = (uint8_t *)malloc(length);
												
												if(data != NULL)
												{
													int read_err = localRecP->reader->Read(blockFrame.pos, blockFrame.len, data);
													
													if(read_err == PrMkvReader::PrMkvSuccess)
													{
														vpx_codec_stream_info_t stream_info;
														stream_info.sz = sizeof(stream_info);
													
														vpx_codec_err_t peek_err = vpx_codec_peek_stream_info(iface, data, length, &stream_info);
														
														if(peek_err == VPX_CODEC_OK)
														{
															if(!first_frame && stream_info.is_kf && got_frame)
															{
																reached_next_iframe = true;
																
																assert(got_frame); // now this is always true, of course, but we thought seek would take us to the iframe before the frame we wanted
															}
														}
														
														if(!reached_next_iframe)
														{
															vpx_codec_err_t decode_err = vpx_codec_decode(&decoder, data, length, NULL, 0);
															
															if(decode_err == VPX_CODEC_OK)
															{
																csSDK_int32 decodedFrame = ((packet_tstamp + half_frame_time) / fps_den) * fps_num / 1000000000UL;
																
																csSDK_int32 hopingforFrame = ((tstamp + half_frame_time) / fps_den) * fps_num / 1000000000UL;
																assert(hopingforFrame == theFrame);
																
																vpx_codec_iter_t iter = NULL;
																
																vpx_image_t *img = vpx_codec_get_frame(&decoder, &iter);
																
																if(img)
																{
																	PPixHand ppix;
																	
																	localRecP->PPixCreatorSuite->CreatePPix(&ppix, PrPPixBufferAccess_ReadWrite, frameFormat->inPixelFormat, &theRect);

																	assert(frameFormat->inPixelFormat == PrPixelFormat_YUV_420_MPEG2_FRAME_PICTURE_PLANAR_8u_709);
																	
																	char *Y_PixelAddress, *U_PixelAddress, *V_PixelAddress;
																	csSDK_uint32 Y_RowBytes, U_RowBytes, V_RowBytes;
																	
																	localRecP->PPix2Suite->GetYUV420PlanarBuffers(ppix, PrPPixBufferAccess_ReadWrite,
																													&Y_PixelAddress, &Y_RowBytes,
																													&U_PixelAddress, &U_RowBytes,
																													&V_PixelAddress, &V_RowBytes);
																												
																	assert(frameFormat->inFrameHeight == img->d_h);
																	assert(frameFormat->inFrameWidth == img->d_w);

																	for(int y = 0; y < img->d_h; y++)
																	{
																		unsigned char *imgY = img->planes[VPX_PLANE_Y] + (img->stride[VPX_PLANE_Y] * y);
																		
																		unsigned char *prY = (unsigned char *)Y_PixelAddress + (Y_RowBytes * y);
																		
																		memcpy(prY, imgY, img->d_w * sizeof(unsigned char));
																	}
																	
																	for(int y = 0; y < img->d_h / 2; y++)
																	{
																		unsigned char *imgU = img->planes[VPX_PLANE_U] + (img->stride[VPX_PLANE_U] * y);
																		unsigned char *imgV = img->planes[VPX_PLANE_V] + (img->stride[VPX_PLANE_V] * y);
																		
																		unsigned char *prU = (unsigned char *)U_PixelAddress + (U_RowBytes * y);
																		unsigned char *prV = (unsigned char *)V_PixelAddress + (V_RowBytes * y);
																		
																		memcpy(prU, imgU, (img->d_w / 2) * sizeof(unsigned char));
																		memcpy(prV, imgV, (img->d_w / 2) * sizeof(unsigned char));
																	}
																	
																	localRecP->PPixCacheSuite->AddFrameToCache(	localRecP->importerID,
																												0,
																												ppix,
																												decodedFrame,
																												NULL,
																												NULL);
																	
																	if(decodedFrame == theFrame)
																	{
																		*sourceVideoRec->outFrame = ppix;
																		
																		got_frame = true;
																	}
																	else
																	{
																		localRecP->PPixSuite->Dispose(ppix);
																	}
																	
																	vpx_img_free(img);
																}
															}
															else
																result = imFileReadFailed;
														}
													}
													else
														result = imFileReadFailed;
													
													free(data);
												}
												else
													result = imMemErr;
											}
											
											first_frame = false;
																			
											long status = pCluster->GetNext(pBlockEntry, pBlockEntry);
											
											assert(status == 0);
										}
										
										pCluster = localRecP->segment->GetNext(pCluster);
									}
									
									vpx_codec_err_t destroy_err = vpx_codec_destroy(&decoder);
									assert(destroy_err == VPX_CODEC_OK);
								}
							}
						}
					}
				}
			}
		}
	}


	stdParms->piSuites->memFuncs->unlockHandle(reinterpret_cast<char**>(ldataH));

	return result;
}


static int PrivateDataCount(const unsigned char *private_data, size_t private_size)
{
	// the first byte
	unsigned char *p = (unsigned char *)private_data;
	
	return *p + 1;
}

static uint64_t
xiph_lace_value(const unsigned char ** np)
{
	uint64_t lace;
	uint64_t value;
	const unsigned char *p = *np;

	lace = *p++;
	value = lace;
	while (lace == 255) {
		lace = *p++;
		value += lace;
	}

	*np = p;

	return value;
}

static const unsigned char *GetPrivateDataPart(const unsigned char *private_data,
												size_t private_size, int part,
												size_t *part_size)
{
	const unsigned char *result = NULL;
	size_t result_size = 0;
	
	const unsigned char *p = private_data;
	
	int count = *p++ + 1;
	assert(count == 3);
	
	
	if(*p >= part)
	{
		uint64_t sizes[3];
		uint64_t total = 0;
		int i = 0;
		
		while(--count)
		{
			sizes[i] = xiph_lace_value(&p);
			total += sizes[i];
			i++;
		}
		sizes[i] = private_size - total - (p - private_data);
		
		for(i=0; i < part; ++i)
			p += sizes[i];
		
		result = p;
		result_size = sizes[part];
	}
	
	*part_size = result_size;
	
	return result;
}
												

static prMALError 
SDKImportAudio7(
	imStdParms			*stdParms, 
	imFileRef			SDKfileRef, 
	imImportAudioRec7	*audioRec7)
{
	prMALError		result		= malNoError;

	// privateData
	ImporterLocalRec8H ldataH = reinterpret_cast<ImporterLocalRec8H>(audioRec7->privateData);
	stdParms->piSuites->memFuncs->lockHandle(reinterpret_cast<char**>(ldataH));
	ImporterLocalRec8Ptr localRecP = reinterpret_cast<ImporterLocalRec8Ptr>( *ldataH );


	assert(localRecP->reader != NULL && localRecP->reader->FileRef() == SDKfileRef);
	assert(localRecP->segment != NULL);
	
	if(localRecP->segment)
	{
		uint64_t tstamp = audioRec7->position * 1000000000UL / localRecP->audioSampleRate;
		
		if(localRecP->audio_track >= 0)
		{
			const mkvparser::Tracks* pTracks = localRecP->segment->GetTracks();
		
			const mkvparser::Track* const pTrack = pTracks->GetTrackByNumber(localRecP->audio_track);
			
			if(pTrack != NULL)
			{
				const long trackType = pTrack->GetType();
			
				if(trackType == mkvparser::Track::kAudio)
				{
					assert(pTrack->GetCodecId() == std::string("A_VORBIS"));
				
					const mkvparser::AudioTrack* const pAudioTrack = static_cast<const mkvparser::AudioTrack*>(pTrack);
					
					if(pAudioTrack)
					{
						size_t private_size = 0;
						const unsigned char *private_data = pAudioTrack->GetCodecPrivate(private_size);
						
						if(private_data && private_size && PrivateDataCount(private_data, private_size) == 3)
						{
							vorbis_info vi;
							vorbis_comment vc;
							vorbis_dsp_state vd;
							vorbis_block vb;
							
							vorbis_info_init(&vi);
							vorbis_comment_init(&vc);
							
							int ogg_packet_num = 0;
							
						#define OV_OK 0
						
							int v_err = OV_OK;
							
							for(int h=0; h < 3 && v_err == OV_OK; h++)
							{
								size_t length = 0;
								const unsigned char *data = GetPrivateDataPart(private_data, private_size,
																				h, &length);
								
								if(data != NULL)
								{
									ogg_packet packet;
									
									packet.packet = (unsigned char *)data;
									packet.bytes = length;
									packet.b_o_s = (h == 0);
									packet.e_o_s = false;
									packet.granulepos = 0;
									packet.packetno = ogg_packet_num++;
									
									v_err = vorbis_synthesis_headerin(&vi, &vc, &packet);
								}
							}
									
							if(v_err == OV_OK)
							{
								v_err = vorbis_synthesis_init(&vd, &vi);
								
								if(v_err == OV_OK)
									v_err = vorbis_block_init(&vd, &vb);
								
								
								const mkvparser::BlockEntry* pSeekBlockEntry = NULL;
								
								pAudioTrack->Seek(tstamp, pSeekBlockEntry);
								
								if(pSeekBlockEntry != NULL)
								{
									csSDK_uint32 samples_copied = 0;
									csSDK_uint32 samples_left = audioRec7->size;
									
									const mkvparser::Cluster *pCluster = pSeekBlockEntry->GetCluster();
									
									while((pCluster != NULL) && !pCluster->EOS() && samples_left > 0 && result == malNoError)
									{
										const mkvparser::BlockEntry* pBlockEntry = NULL;
										
										pCluster->GetFirst(pBlockEntry);
										
										while((pBlockEntry != NULL) && !pBlockEntry->EOS() && samples_left > 0 && result == malNoError)
										{
											const mkvparser::Block *pBlock = pBlockEntry->GetBlock();
											
											if(pBlock->GetTrackNumber() == localRecP->audio_track)
											{
												long long packet_tstamp = pBlock->GetTime(pCluster);
												
												PrAudioSample packet_start = localRecP->audioSampleRate * packet_tstamp / 1000000000UL;
												
												PrAudioSample packet_offset = audioRec7->position - packet_start; // in other words the audio frames in the beginning that we'll skip over
												
												if(packet_offset < 0)
													packet_offset = 0;
													
												for(int f=0; f < pBlock->GetFrameCount(); f++)
												{
													const mkvparser::Block::Frame& blockFrame = pBlock->GetFrame(f);
													
													unsigned int length = blockFrame.len;
													uint8_t *data = (uint8_t *)malloc(length);
													
													if(data != NULL)
													{
														int read_err = localRecP->reader->Read(blockFrame.pos, blockFrame.len, data);
														
														if(read_err == PrMkvReader::PrMkvSuccess)
														{
															ogg_packet packet;
										
															packet.packet = data;
															packet.bytes = length;
															packet.b_o_s = false;
															packet.e_o_s = false;
															packet.granulepos = -1;
															packet.packetno = ogg_packet_num++;

															int synth_err = vorbis_synthesis(&vb, &packet);
															
															if(synth_err == OV_OK)
															{
																int block_err = vorbis_synthesis_blockin(&vd, &vb);
																
																if(block_err == OV_OK)
																{
																	float **pcm = NULL;
																	int samples;
																	
																	int synth_result = 1;
																	
																	while(synth_result != 0 && (samples = vorbis_synthesis_pcmout(&vd, &pcm)) > 0)
																	{
																		int samples_to_copy = samples_left;
																		
																		if(packet_offset >= samples)
																		{
																			samples_to_copy = 0;
																		}
																		else if(samples_to_copy > (samples - packet_offset))
																		{
																			samples_to_copy = (samples - packet_offset);
																		}
																	
																		// how nice, audio samples are float, just like Premiere wants 'em
																		for(int c=0; c < localRecP->numChannels && samples_to_copy > 0; c++)
																		{
																			memcpy(audioRec7->buffer[c] + samples_copied, pcm[c] + packet_offset, samples_to_copy * sizeof(float));
																		}
																		
																		samples_copied += samples_to_copy;
																		samples_left -= samples_to_copy;
																		
																		if(samples_to_copy > 0)
																			packet_offset = 0;
																		else
																			packet_offset -= samples;
																		
																		
																		synth_result = vorbis_synthesis_read(&vd, samples);
																	}
																}
																else
																	result = imFileReadFailed;
															}
															else
																result = imFileReadFailed;
														}
														else
															result = imFileReadFailed;
														
														free(data);
													}
													else
														result = imMemErr;
												}
											}
											
											long status = pCluster->GetNext(pBlockEntry, pBlockEntry);
											
											assert(status == 0);
										}
										
										pCluster = localRecP->segment->GetNext(pCluster);
									}

									// actually, there might be samples left at the end, not much we can do about that
									//assert(samples_left == 0 && samples_copied == audioRec7->size);
								}
								else
									result = imFileReadFailed;
							}
							else
								result = imFileReadFailed;
							
							
							vorbis_block_clear(&vb);
							vorbis_dsp_clear(&vd);
							vorbis_info_clear(&vi);
							vorbis_comment_clear(&vc);
						}
						else
							result = imFileReadFailed;
					}
					else
						result = imFileReadFailed;
				}
				else
					result = imFileReadFailed;
			}
			else
				result = imFileReadFailed;
		}
	}
	
					
	stdParms->piSuites->memFuncs->unlockHandle(reinterpret_cast<char**>(ldataH));
	
	assert(result == malNoError);
	
	return result;
}


PREMPLUGENTRY DllExport xImportEntry (
	csSDK_int32		selector, 
	imStdParms		*stdParms, 
	void			*param1, 
	void			*param2)
{
	prMALError	result				= imUnsupported;

	try{

	switch (selector)
	{
		case imInit:
			result =	SDKInit(stdParms, 
								reinterpret_cast<imImportInfoRec*>(param1));
			break;

		case imGetInfo8:
			result =	SDKGetInfo8(stdParms, 
									reinterpret_cast<imFileAccessRec8*>(param1), 
									reinterpret_cast<imFileInfoRec8*>(param2));
			break;

		case imOpenFile8:
			result =	SDKOpenFile8(	stdParms, 
										reinterpret_cast<imFileRef*>(param1), 
										reinterpret_cast<imFileOpenRec8*>(param2));
			break;
		
		case imQuietFile:
			result =	SDKQuietFile(	stdParms, 
										reinterpret_cast<imFileRef*>(param1), 
										param2); 
			break;

		case imCloseFile:
			result =	SDKCloseFile(	stdParms, 
										reinterpret_cast<imFileRef*>(param1), 
										param2);
			break;

		case imAnalysis:
			result =	SDKAnalysis(	stdParms,
										reinterpret_cast<imFileRef>(param1),
										reinterpret_cast<imAnalysisRec*>(param2));
			break;

		case imGetIndFormat:
			result =	SDKGetIndFormat(stdParms, 
										reinterpret_cast<csSDK_size_t>(param1),
										reinterpret_cast<imIndFormatRec*>(param2));
			break;

		case imSaveFile8:
			result =	SDKSaveFile8(	stdParms, 
										reinterpret_cast<imSaveFileRec8*>(param1));
			break;
			
		case imDeleteFile8:
			result =	SDKDeleteFile8(	stdParms, 
										reinterpret_cast<imDeleteFileRec8*>(param1));
			break;

		case imGetIndPixelFormat:
			result = SDKGetIndPixelFormat(	stdParms,
											reinterpret_cast<csSDK_size_t>(param1),
											reinterpret_cast<imIndPixelFormatRec*>(param2));
			break;

		// Importers that support the Premiere Pro 2.0 API must return malSupports8 for this selector
		case imGetSupports8:
			result = malSupports8;
			break;

		case imCalcSize8:
			result =	SDKCalcSize8(	stdParms,
										reinterpret_cast<imCalcSizeRec*>(param1),
										reinterpret_cast<imFileAccessRec8*>(param2));
			break;

		case imGetPreferredFrameSize:
			result =	SDKPreferredFrameSize(	stdParms,
												reinterpret_cast<imPreferredFrameSizeRec*>(param1));
			break;

		case imGetSourceVideo:
			result =	SDKGetSourceVideo(	stdParms,
											reinterpret_cast<imFileRef>(param1),
											reinterpret_cast<imSourceVideoRec*>(param2));
			break;
			
		case imImportAudio7:
			result =	SDKImportAudio7(	stdParms,
											reinterpret_cast<imFileRef>(param1),
											reinterpret_cast<imImportAudioRec7*>(param2));
			break;

		case imCreateAsyncImporter:
			result =	imUnsupported;
			break;
	}
	
	}catch(...) { result = imOtherErr; }

	return result;
}

