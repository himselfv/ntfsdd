#include <windows.h>
#include <winioctl.h>
#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include "ntfs.h"
#include "util.h"
#include "ntfsvolume.h"
#include "ntfsmft.h"
#include "bitmap.h"
#include "mftdiff.h"
#include "clusterdiff.h"
#include "vssutil.h"
#include <CLI/CLI.hpp>



template<typename Enum>
struct EnumItemInfo {
	Enum value;
	std::string name;
	std::string desc;
};

template<typename Enum>
struct EnumNames {};

template<typename Enum, decltype(EnumNames<Enum>::info)* names = nullptr>
inline
std::map<std::string, Enum> enumMap() {
	std::map<std::string, Enum> result {};
	for (auto& info : EnumNames<Enum>::info())
		result.emplace(info.name, info.value);
	return result;
}

template<typename Enum, decltype(EnumNames<Enum>::info)* names = nullptr>
inline
std::string enumName(Enum value) {
	for (auto& pair : EnumNames<Enum>::info())
		if (pair.value == value) return pair.name;
	return std::string{};
}

template<typename Enum, decltype(EnumNames<Enum>::info)* names = nullptr>
inline
std::string enumNames(const std::string& separator) {
	std::string result = {};
	for (auto& pair : EnumNames<Enum>::info())
		result += pair.name + separator;
	result.resize(result.size() - separator.size());
	return result;
}

template<typename Enum, decltype(EnumNames<Enum>::info)* names = nullptr>
inline
std::string enumDescriptions(const std::string& separator) {
	std::string result = {};
	for (auto& pair : EnumNames<Enum>::info())
		result += pair.desc + separator;
	result.resize(result.size() - separator.size());
	return result;
}

template<typename Enum, decltype(EnumNames<Enum>::info)* names = nullptr>
inline
std::string enumNameDesc(const std::string& nameDescSeparator, const std::string& itemSeparator) {
	std::string result = {};
	for (auto& pair : EnumNames<Enum>::info())
		result += pair.name + nameDescSeparator + pair.desc + itemSeparator;
	result.resize(result.size() - itemSeparator.size());
	return result;
}



enum class DdAction : int { List, Compare, Copy, Rcw, VerifyBitmap };
template<> struct EnumNames<DdAction> {
	typedef std::vector<EnumItemInfo<DdAction>> Info;
	static const Info info() {
		static const Info m{
			{ DdAction::List,	 "list",	"List candidate sectors" },
			{ DdAction::Compare, "compare",	"Compare candidate sectors and print the differences"  },
			{ DdAction::Copy,	 "copy",	"Copy all candidate sectors"  },
			{ DdAction::Rcw,	 "rcw",		"Compare candidate sectors and copy the changed ones"  },
			{ DdAction::VerifyBitmap, "verifyBitmap", "Rebuild $Bitmap from MFT and compare to the actual one"  },
		};
		return m;
	}
};
std::ostream& operator<<(std::ostream &os, const DdAction &value) {
	return (os << enumName(value));
}

enum class DdMode : int { All, Bitmap, MFT };
template<> struct EnumNames<DdMode> {
	typedef std::vector<EnumItemInfo<DdMode>> Info;
	static const Info info() {
		const Info m{
			{ DdMode::All,	  "all",		"All clusters" },
			{ DdMode::Bitmap, "bitmap",		"All clusters in use at source according to $Bitmap"  },
			{ DdMode::MFT,	  "mft",		"All sectors in use at source according to MFT segments that differ from destination"  },
		};
		return m;
	}
};
std::ostream& operator<<(std::ostream &os, const DdMode &value) {
	return (os << enumName(value));
}

/*
enum class DdTrim : int { None, Changes, All };
template<> struct EnumNames<DdTrim> {
	typedef std::vector<EnumItemInfo<DdMode>> Info;
	static const Info info() {
		const Info m{
			{ DdTrim::None, "none", "" },
			{ DdTrim::Changes, "changes", "" },
			{ DdTrim::All, "all", "" },
		};
		return m;
	}
};
std::ostream& operator<<(std::ostream &os, const DdTrim &value) {
	return (os << enumName(value));
}
*/



/*
Things we could receive from the user:

Volume GUIDs:
    \\?\Volume{GUID}\
Good for reading and writing, but as a safety I want to ensure write targets have no mount points.
GetVolumePathNamesForVolumeName will give us all mount points.


Drive letters and mount points:
	D:
	D:\Path
Good for reading and writing, but as a safety - see above: no writing unless explicitly overriden.
GetVolumeNameForVolumeMountPoint will find its volume GUID.

Windows is already very picky about mounted volumes. You will likely need a shadow copy as a source.


Shadow copies:
	\\?\GLOBALROOT\Device\HarddiskVolumeShadowCopy...
Good for reading, unsuitable for writing.

Mounted shadow copies!
	T:
	D:\ShadowCopies\Abcd
GetVolumeNameForVolumeMountPoint will not work.
QueryDosDevice might resolve drive letter, GetFinalPathNameByHandleW a mount point (reparse point).
But I don't really want to get so involved.


VHDs:
When loaded, behave more or less like normal volumes, but:
- FSCTL_LOCK_VOLUME fails!
Can be opened as files. Contain raw disk data + footer.
You can use --no-lock-*, but maybe its easier to access these as files.
PROBLEM:
- VHD is not a volume but a disk. It contains the partition table + some number of partitions.


Files:
	D:\Path\disk.img
Surprisingly fitting for our goals. But the entire space has to be reserved in the file.

Files do not support FSCTL_LOCK_VOLUME (whatever), but also no FSCTL_GET_VOLUME_DATA or FSCTL_GET_VOLUME_BITMAP,
so we'll have to make exceptions in all these cases if we want to support files.


PhysicalDrive and Partition objects:
	\\.\PhysicalDrive0
	\\.\Harddisk0Partition1
Not the same thing as volumes, and we are likely better off not messing with them.


Let's try not to be too smart and to allow passing anything and then just try to work with it.
*/

/*
Resolves any volume mount point to a volume GUID path:
  \\?\Volume{GUID}\
Only works for normal volumes, not VSS shadows.
*/
bool resolveVolumeGuid(const std::string& volumePath, std::string& volumeGuid)
{
	auto volumePathNormalized = volumePath;
	if (!volumePathNormalized.empty() && (volumePathNormalized.back() != '\\'))
		volumePathNormalized += '\\';
	volumeGuid.resize(50); // GUID paths are fixed length
	if (!GetVolumeNameForVolumeMountPointA(volumePathNormalized.c_str(), &volumeGuid[0], 50)) {
		auto err = GetLastError();
		if (err = ERROR_INVALID_NAME)
			return false;
		throwOsError(err, std::string("Cannot resolve a volume mount point: ")+volumePath);
	}
	return true;
}

std::vector<std::string> getVolumeMountPoints(const std::string& volumeGuid)
{
	std::vector<std::string> results;
	DWORD bufferLength = 0;

	auto volumeGuidNormalized = volumeGuid;
	if (!volumeGuidNormalized.empty() && (volumeGuidNormalized.back() != '\\'))
		volumeGuidNormalized += '\\';

	if (!GetVolumePathNamesForVolumeNameA(volumeGuidNormalized.c_str(), NULL, 0, &bufferLength)) {
		auto err = GetLastError();
		if (err != ERROR_MORE_DATA)
			throwOsError(err, "GetVolumePathNamesForVolumeName");
	}

	std::vector<char> buffer(bufferLength);

	if (!GetVolumePathNamesForVolumeNameA(volumeGuidNormalized.c_str(), buffer.data(), bufferLength, &bufferLength))
		throwLastOsError("GetVolumePathNamesForVolumeName");

	//Parse the multistring buffer
	char* currentPath = buffer.data();
	while (*currentPath != '\0') {
		results.push_back(std::string(currentPath));
		currentPath += strlen(currentPath) + 1;
	}
	return results;
}

/*
Prints mount points/verifies there are none, according to the flags passed.
Returns true if the path is indeed a volume mount point or its GUID path.
*/
bool verifyMountPoints(const std::string& volumePath, bool printMountPoints, bool requireNoMountpoints)
{
	std::string volumeGuid {};
	if (!resolveVolumeGuid(volumePath, volumeGuid)) {
		//Okay, we tried
		if (printMountPoints)
			std::cerr << volumePath << " doesn't seem to be a standard volume with mount point support." << std::endl;
		return false; //Doesn't seem to be a standard volume, can't help with mount points
	}

	std::vector<std::string>  {};
	auto mountPoints = getVolumeMountPoints(volumeGuid);

	if (mountPoints.empty()) {
		if (printMountPoints)
			std::cerr << "Volume " << volumeGuid << " has no mount points." << std::endl;
	} else
	{
		if (printMountPoints || requireNoMountpoints) {
			std::cerr << "Volume " << volumeGuid << " mount points:" << std::endl;
			for (auto& mountPoint : mountPoints)
				std::cerr << "  " << mountPoint << std::endl;
		}
		if (requireNoMountpoints)
			throw std::runtime_error(volumePath + std::string{ " has active mount points. Check that this is really the volume you meant. Dismount beforehand and use its GUID path." });
	}
	return true;
}


//Volume + its MFT.
class Volume2 : public Volume {
public:
	Mft mft = { this };
	using Volume::Volume;
};

void compareVolumeParams(Volume& a, Volume& b, bool safety_override)
{
	auto& avd = a.volumeData();
	auto& bvd = b.volumeData();
	auto safetyTest2 = [safety_override](bool condition, const std::string& message) {
		if (condition) return;
		if (safety_override) std::cerr << "WARNING: Volumes differ: " << message;
		else throw std::runtime_error(std::string{"Volumes differ: "}+message);
	};
	auto safetyTest = [safety_override](uint64_t val_a, uint64_t val_b, const std::string& message) {
		if (val_a == val_b) return;
		std::string fullMessage = std::string{ "Volumes differ: " }+message
			+ std::string{ ". A=" } +std::to_string(val_a)
			+ std::string{ ", B=" } +std::to_string(val_b);
		if (safety_override) std::cerr << "WARNING: " << fullMessage;
		else throw std::runtime_error(fullMessage);
	};

	safetyTest(avd.BytesPerCluster, bvd.BytesPerCluster, "BytesPerCluster");
	safetyTest(avd.BytesPerFileRecordSegment, bvd.BytesPerFileRecordSegment, "BytesPerFileRecordSegment");
	safetyTest(avd.BytesPerSector, bvd.BytesPerSector, "BytesPerSector");
	safetyTest(avd.ClustersPerFileRecordSegment, bvd.ClustersPerFileRecordSegment, "ClustersPerFileRecordSegment");
	safetyTest(avd.TotalClusters.QuadPart, bvd.TotalClusters.QuadPart, "TotalClusters");
	safetyTest(avd.NumberSectors.QuadPart, bvd.NumberSectors.QuadPart, "NumberSectors");
//	safetyTest(avd.MftZoneStart.QuadPart, bvd.MftZoneStart.QuadPart, "MftZoneStart"); //Can differ for some reason!
	safetyTest(avd.MftStartLcn.QuadPart, bvd.MftStartLcn.QuadPart, "MftStartLcn");
	safetyTest(avd.Mft2StartLcn.QuadPart, bvd.Mft2StartLcn.QuadPart, "Mft2StartLcn");
	safetyTest(avd.VolumeSerialNumber.QuadPart, bvd.VolumeSerialNumber.QuadPart, "VolumeSerialNumber");
}


void countClusters(VOLUME_BITMAP_BUFFER* bitmapBuffer)
{
	LARGE_INTEGER numClusters = bitmapBuffer->BitmapSize;

	int64_t clusters_used = 0;
	int64_t clusters_free = 0;
	for (LONGLONG i = 0; i < numClusters.QuadPart; i++) {
		// Find the bit in the byte array
		bool isAllocated = (bitmapBuffer->Buffer[i / 8] & (1 << (i % 8))) != 0;
		if (isAllocated)
			clusters_used++;
		else
			clusters_free++;
	}
	printf("Used: %I64d, free: %I64d\n", clusters_used, clusters_free);
}

void verifyMftLayout(Volume& vol, Mft& mft, const VOLUME_BITMAP_BUFFER* bitmap)
{
	//Verify that the MFT covers more or less the entire volume
	auto& volData = vol.volumeData();

	//Sizes in sectors and in clusters may differ up to one of whichever is bigger.
	int64_t totalBytes = volData.NumberSectors.QuadPart*volData.BytesPerSector;
	auto totalBytesDiff = totalBytes - volData.TotalClusters.QuadPart*volData.BytesPerCluster;
	assert(totalBytesDiff < ((volData.BytesPerCluster > volData.BytesPerSector) ? volData.BytesPerCluster : volData.BytesPerSector));

	if (bitmap != nullptr)
		assert(volData.TotalClusters.QuadPart == bitmap->StartingLcn.QuadPart + bitmap->BitmapSize.QuadPart);
}


//Rebuilds volume bitmap from first principles (from the MFT).
void rebuildVolumeBitmap(Volume& vol, Mft& mft, BitmapBuf* bmp)
{
	bmp->resize(vol.volumeData().TotalClusters.QuadPart);

	auto totalSegments = vol.volumeData().MftValidDataLength.QuadPart / vol.volumeData().BytesPerFileRecordSegment;
	SegmentNumber idx = 0;
	for (auto& segment : SegmentIter(&mft)) {
		if (!mft.isValidSegment(&segment)) continue;
		if ((segment.Flags & FILE_RECORD_SEGMENT_IN_USE) == 0) continue;
		for (auto& attr : AttributeIterator(&segment)) {
			if (attr.FormCode != NONRESIDENT_FORM) continue;
			for (auto& run : DataRunIterator(&attr, DRI_SKIP_SPARSE))
				bmp->set(run.offset, run.offset + run.length - 1);
		}
		if (idx % 1000 == 0) std::cerr << idx << " / " << totalSegments << std::endl;
		idx++;
	}
}


bool compareBitmaps(const VOLUME_BITMAP_BUFFER* bmp1, const Bitmap* bmp2)
{
	if (bmp1->StartingLcn.QuadPart % (sizeof(int64_t) * 8) != 0)
		throw std::runtime_error("StartingLcn is not a multiple of a sufficiently beautiful number, I didn't expect that!");
	return 0==Bitmap::memcmp(bmp1->Buffer, bmp2->data, bmp1->BitmapSize.QuadPart, 0, bmp1->StartingLcn.QuadPart);
/*
	Slow but more elaborate version:
	auto diff = Bitmap(srcBitmap.buf->Buffer, srcBitmap.buf->BitmapSize.QuadPart) ^ srcUsed;
	auto ret = diff.isZero();
	if (!ret)
		diff.print();
	return ret;
*/
}


/*
Safety: Verify that our candidate selection contains at least all the clusters that are set *only in the newer* bitmap.
*/
void verifySelectionContainsNewClusters(CandidateClusterMap& srcSelect, const Bitmap& newlyUsedClusters)
{
	//srcSelect is also a Bitmap so we can do a handy mass op here.
	auto remainder = newlyUsedClusters.andNot(srcSelect);
	if (!remainder.isZero())
		throw std::runtime_error("Assertion failed: some of the newly used clusters are not covered by the constructed difference map!");
}



int main2(int argc, char* argv[]) {
	CLI::App app{ };
	app.name("ntfsdd");
	app.description(R"(NTFS Disk Destroyer.
Compares and updates NTFS volume clones in a dangerously efficient fashion.)");
	app.get_formatter()->column_width(10);
//	app.get_formatter()->enable_description_formatting(false);
	app.get_formatter()->description_paragraph_width(100);

	DdAction action{ DdAction::Compare };
	app.add_option("action", action, "Action to take:\n  "+ enumNameDesc<DdAction>(":\t", "\n  "))
		->type_name(enumNames<DdAction>("|"))
		->transform(CLI::CheckedTransformer(enumMap<DdAction>(), CLI::ignore_case).description(""))
		->capture_default_str()
		;

	DdMode mode{ DdMode::MFT };
	app.add_option("--select", mode, "Selection method to use:\n " + enumNameDesc<DdMode>(":\t", "\n  "))
		->type_name(enumNames<DdMode>("|"))
		->transform(CLI::CheckedTransformer(enumMap<DdMode>(), CLI::ignore_case).description(""))
		->capture_default_str()
		;

/*
	Do you really want us messing with trim? No, you don't. Do defrag /retrim from time to time.

	DdTrim trim{ (DdTrim)(-1) };
	app.add_option("--trim", trim, "Trim unused sectors:\n "+enumNameDesc<DdTrim>(":\t", "\n  "))
		->group("Main options")
		->type_name("none|changes|all")
		->transform(CLI::CheckedTransformer(enumMap<DdTrim>(), CLI::ignore_case).description(""))
		->capture_default_str()
		;
*/

	std::string srcPath, destPath;
	app.add_option("source, --source", srcPath, "Source device/file")
		->required();
	app.add_option("destination, --dest", destPath, "Target device/file")
		->required();


	//Create a temporary VSS shadow copy for the source.
	bool bVssCreateSourceShadow = false;
	app.add_flag("--shadow", bVssCreateSourceShadow,
		"Create a temporary VSS shadow copy for the source. See VSS docs for which paths can be shadowed. "
		"If you pass a manually-created shadow, do not set this flag."
		)
		->group("Access options")
		->capture_default_str()
		;
	bool bVssWritersParticipation = false;
	app.add_flag("--shadow-writers", bVssWritersParticipation,
		"Use VSS_CTX_BACKUP instead of VSS_CTX_FILE_SHARE_BACKUP. Requires --shadow. Read the docs. Better more stable backup, but slower and more flaky shadow creation process itself."
	)
		->group("Access options")
		->capture_default_str()
		;



	//We'll enforce FSCTL_LOCK_VOLUME where it seems reasonable and TRY it elsewhere.
	//These flags force us to insist on it even if we're not sure it should work.
	bool bForceLockSrc = false;
	bool bForceLockDest = false;
	app.add_flag("--force-lock-src", bForceLockSrc, "Force FSCTL_LOCK_VOLUME on the source even when it doesn't seem to be a volume.")
		->group("Access options")
		->capture_default_str()
		;
	app.add_flag("--force-lock-dest", bForceLockDest, "Force FSCTL_LOCK_VOLUME on the destination even when it doesn't seem to be a volume.")
		->group("Access options")
		->capture_default_str()
		;

	bool bAllowWriteMounted = false;
	app.add_flag("--unsafe-allow-mounted", bAllowWriteMounted,
		"Allow the destination volume to have drive letters and mount points. It is advised to never use this switch.\n"
		"Only write to the volumes that you have manually dismounted, to prevent accidental overwriting of unintended volumes."
		)
		->group("Access options")
		->capture_default_str()
		;

	bool bAllowNonMatching = false;
	app.add_flag("--unsafe-allow-non-matching", bAllowNonMatching,
		"Continue even if the destination does not seem to be a clone of the source. You will likely wreak havoc "
		"and destroy unrelated volume by mistake, unless in a very well understood special case."
		)
		->group("Access options")
		->capture_default_str()
		;

	//Separate flag for skipping MFT checks in copy all mode only, as there it sometimes makes sense.
	bool bBlankOverwrite = false;
	app.add_flag("--overwrite", bBlankOverwrite,
		"Overwrite destination completely. Skip destination format checks. Only works for action==copy, selection==all|bitmap. "
		"Without this copy all/bitmap will still check the destination MFT."
		)
		->group("Access options")
		->capture_default_str()
		;

	bool write_mode = false;
	app.add_flag("--write", write_mode, "Write to destination. Final safety. Without this flag we will not actually open the destination as writeable.")
		->group("Access options")
		->capture_default_str()
		;


	std::unordered_set<SegmentNumber> skipSegments{};
	app.add_option("--skip-segments", skipSegments, "Skip MFT entries with these numbers. Only works for MFT modes. The segments are still copied, the data is skipped.")
		->group("Processing options");

	LCN asyncBatchLen = 160;
	app.add_option("--batch-len", asyncBatchLen, "Max batch length, in clusters, for reading.")
		->group("Processing options")
		->capture_default_str();

	int asyncQueueDepth = 16;
	app.add_option("--queue-depth", asyncQueueDepth, "Queue depth, in parallel outstanding requests, for reading.")
		->group("Processing options")
		->capture_default_str();




	//In List mode, prints selected clusters. In Compare/Rvw modes prints changed clusters.
	ClusterPrinter clusterPrinter;
	app.add_option("--print-clusters", clusterPrinter.outputFile, "In List and Compare modes, print selected and matching clusters respectively.")
		->group("Output options")
		->expected(0,0)
		->default_str("-")
		;

	app.add_option("--print-clusters-to", clusterPrinter.outputFile, "Same as --print-clusters but allows you to specify a file.")
		->group("Output options")
		;

	app.add_flag("--cluster-spans", clusterPrinter.clustersAsSpans, "For modes that print cluster lists, print cluster spans instead of individual clusters.")
		->group("Output options")
		->capture_default_str()
		;

	app.add_option("--cluster-separator", clusterPrinter.separator, "For modes that print cluster lists, use this as a separator (\\n etc allowed).")
		->transform(CLI::EscapedString)
		->group("Output options")
		;


	//In List mode, this is going to print *selected* files. In Compare/Rvw, the files with actual *differences* (interspersed with clusters)
	//Note that this will not print DELETED files. The comparison is one-way.
	FilePrinter filenamePrinter;
	app.add_flag("--print-files", filenamePrinter.outputFile, "In List and Compare modes, print files and dirs which contain selected (List) and matching (Compare) clusters, respectively.")
		->group("Output options")
		->expected(0, 0)
		->default_str("-")
		;
	app.add_option("--print-files-to", filenamePrinter.outputFile, "Same as --print-files but allows you to specify a file.")
		->group("Output options")
		;


	bool bReturnExitCode = false;
	app.add_flag("--exit-code", bReturnExitCode, "Return non-zero exit code if there were differences (compare/rvw) or the selection had been non nil (list/copy). By default exit code is non-zero only on failures.")
		->group("Output options")
		->capture_default_str()
		;


	bool progress = false;
	app.add_flag("--progress", progress, "Display operations progress.")
		->group("Output options")
		->capture_default_str()
		;

	bool verbose = false;
	app.add_flag("--verbose", verbose, "Detailed logging.")
		->group("Output options")
		->capture_default_str()
		;

	CLI11_PARSE(app, argc, argv);

	bool bNeedsWrites = (action == DdAction::Copy || action == DdAction::Rcw);

	//Skip destination format checks.
	bool bBlankTarget = (action == DdAction::Copy && (mode == DdMode::All||mode==DdMode::Bitmap)) && bBlankOverwrite;

	if (bVssWritersParticipation && !bVssCreateSourceShadow)
		std::cerr << "Warning: --shadow-writers without --shadow, ignored." << std::endl;


	// Before we open handles, auto-create the shadow
	std::unique_ptr<VssShadowCopy> srcShadow;
	if (bVssCreateSourceShadow) {
		//This initializes COM so only try to create when asked to.
		srcShadow.reset(new VssShadowCopy());
		srcShadow->setSnapshotMode(bVssWritersParticipation ? VssSnapshotMode::WriterBackup : VssSnapshotMode::NonWriterBackup);
		std::cerr << "VSS: Creating shadow copy for " << srcPath << std::endl;
		srcShadow->create(utf8ToWchar(srcPath));
		auto snapshotPath = wcharToUtf8(srcShadow->snapshotPath());
		std::cerr << "VSS: Shadow copy for " << srcPath << " created at: " << snapshotPath << std::endl;
		srcPath = snapshotPath;
	}


	if (verbose) {
		std::cerr << "SOURCE: " << srcPath << std::endl;
		std::cerr << "DEST: " << destPath << std::endl;
	}

	// Open Handles
	bool srcIsVolume = verifyMountPoints(srcPath, verbose, false);
	auto src = Volume2(srcPath, GENERIC_READ);
	src.readLayout();
	if (srcIsVolume)
		src.verifyPhysicalVolumeParams();

	bool destIsVolume = verifyMountPoints(destPath, verbose, bNeedsWrites && !bAllowWriteMounted);
	DWORD dwDestMode = GENERIC_READ;
	if (write_mode && bNeedsWrites)
		dwDestMode |= GENERIC_WRITE;
	auto dest = Volume2(destPath, dwDestMode);

	//If we're in blind copy mode, we should not have to read the destination layout
	if (bBlankTarget)
		dest.initLayout(src.volumeData(), src.extendedVolumeData());
	else {
		dest.readLayout();
		if (destIsVolume)
			dest.verifyPhysicalVolumeParams();
		compareVolumeParams(src, dest, bAllowNonMatching);
	}


	// 2. Prepare Target (Lock and Dismount)
	std::cerr << "Locking src..." << std::endl;
	VolumeLock srcLock(src, !srcIsVolume && !bForceLockSrc);
	std::cerr << "Locking dest..." << std::endl;
	VolumeLock dstLock(dest, !destIsVolume && !bForceLockDest);

	/*
	Do not dismount.
	Dismounting triggers various system activity for 5-8 seconds which prevents locking.
	Locking first works, but fails if you re-run the app too soon after a succesfull lock+dismount due to the activity still going on.
	
	But we don't really need to dismount, as NTFS "treats locked volumes as dismounted".
	So we're already in a more powerful state.
	DWORD bytesReturned;
	std::cerr << "Dismounting dest..." << std::endl;
	if (!dest.ioctl(FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &bytesReturned, NULL))
		throwLastOsError("FSCTL_DISMOUNT_VOLUME");
	*/

	// Open and scan MFT
	std::cerr << "Loading MFT structures..." << std::endl;
	src.mft.load();
	if (!bBlankTarget)
		dest.mft.load();
	else
		dest.mft.loadMinimal();

	std::cerr << "Loading stored bitmap..." << std::endl;
	NtfsBitmapFile srcBitmap(&src, &src.mft);
	std::unique_ptr<NtfsBitmapFile> destBitmap{ nullptr };
	if (!bBlankTarget)
		destBitmap.reset(new NtfsBitmapFile(&dest, &dest.mft));

	std::cerr << "Verifying MFT layouts..." << std::endl;
	verifyMftLayout(src, src.mft, srcBitmap.buf);
	if (!bBlankTarget)
		verifyMftLayout(dest, dest.mft, nullptr);

	BitmapBuf srcUsed;
	CandidateClusterMap srcSelect;
	MftDiff::Filemap filemap;

	if (action == DdAction::VerifyBitmap) {
		std::cerr << "Recalculating $Bitmap..." << std::endl;
		auto t1 = GetTickCount();
		rebuildVolumeBitmap(src, src.mft, &srcUsed);
		std::cerr << "Time: " <<  (GetTickCount() - t1) << std::endl;
	}
	if (action == DdAction::Copy || action == DdAction::List || action == DdAction::Compare || action == DdAction::Rcw) {
		std::cerr << "Building file table bitmaps..." << std::endl;
		auto t1 = GetTickCount();
		switch (mode) {
		case DdMode::All:
			srcSelect.resize(src.volumeData().TotalClusters.QuadPart);
			srcSelect.set(ClusterRun{ 0, src.volumeData().TotalClusters.QuadPart });
			break;
		case DdMode::Bitmap:
			srcSelect.resize(src.volumeData().TotalClusters.QuadPart);
			for (auto& run : BitmapSpans((uint64_t*)srcBitmap.buf->Buffer, srcBitmap.buf->BitmapSize.QuadPart))
				srcSelect.set(run);
			break;
		case DdMode::MFT: {
			std::cerr << "Reading MFT segments..." << std::endl;
			ConsoleProgressCallback progressCallback("Reading MFT");
			progressCallback.setOnceEvery(1000);
			MftDiff diff(src.mft, dest.mft);
			diff.skipSegments(skipSegments);
			diff.filemapNeedNames = !filenamePrinter.active();
			diff.filemapListDirty = !filenamePrinter.active();
			diff.progressCallback = &progressCallback;
			diff.scan();
			srcSelect = std::move(diff.srcDiff);
			filemap = std::move(diff.filemap);
			std::cerr << "Used segments: " << diff.stats.usedSegments << std::endl;
			std::cerr << "Dirty segments: " << diff.stats.dirtySegments << std::endl;
			if (verbose)
				std::cerr << "Multisegments: " << diff.stats.multiSegments << std::endl;
			break;
		}
		}
		std::cerr << "Time: " << (GetTickCount() - t1) << std::endl;
	}


	//Verify our manual cluster usage map matches $Bitmap, if we have it from VerifyBitmap or any task with select==MFT.
	//Be quiet on success in non-VerifyBitmap modes.
	if (srcUsed.size > 0) {
		if (verbose)
			std::cerr << "Verifying file table bitmap..." << std::endl;
		auto cmp = compareBitmaps(srcBitmap.buf, &srcUsed);
		if (!cmp)
			throw std::runtime_error(std::string{ "Manually constructed bitmap is not identical to the NTFS one!" });
	}


	LCN candidateClusterCount = 0;

	if (action == DdAction::List || action == DdAction::Copy || action == DdAction::Compare || action == DdAction::Rcw) {
		//Dirty clusters after selection
		if ((action == DdAction::List || action == DdAction::Copy))
			clusterPrinter.print(srcSelect);
		candidateClusterCount = srcSelect.bitCount();
		std::cerr << "Selected cluster count: " << candidateClusterCount << std::endl;

		//Safety: Verify that our resulting list contains all clusters unique to srcBitmap (that is, switched to 1 since destBitmap).
		//If $Bitmap shows a block was turned from free to used, stop. That should not happen if I'm parsing MFT correctly.
		auto t1 = GetTickCount();
		if (bBlankTarget)
			verifySelectionContainsNewClusters(srcSelect, srcBitmap.asBitmap());
		else
			verifySelectionContainsNewClusters(srcSelect, srcBitmap.asBitmap().andNot(destBitmap->asBitmap()));
		std::cerr << "Time: " << (GetTickCount() - t1) << std::endl;
	}


	//Dirty files after selection
	if ((action == DdAction::List || action == DdAction::Copy) && !filenamePrinter.active()) {
		filenamePrinter.open();
		for (auto& fi : filemap)
			if (fi.second.dirty) {
				std::string filename = fi.second.filename;
				if (filename.empty())
					filename = std::string{ "#" }+std::to_string(fi.first);
				filenamePrinter.printOne(filename + "\t" + std::to_string(fi.second.totalClusters));
			}
	}


	LCN diffClusterCount = 0;
	BitmapBuf srcDiff {};

	std::unique_ptr<ClusterProcessor> clproc{};
	ConsoleProgressCallback progressCallback("Processing");
	progressCallback.setOnceEvery(50000);
	if (action == DdAction::Copy) {
		clproc.reset(new ClusterCopier(src, dest));
		progressCallback.setOperationName("Copying");
	} else if (action == DdAction::Compare) {
		clproc.reset(new ClusterDiffComparer(src, dest));
		progressCallback.setOperationName("Comparing");
	} else if ( action == DdAction::Rcw) {
		clproc.reset(new ClusterDiffWriter(src, dest));
		progressCallback.setOperationName("Rcw");
	}
	clproc->ASYNC_BATCH_LEN = asyncBatchLen;
	clproc->ASYNC_QUEUE_DEPTH = asyncQueueDepth;
	if (clproc) {
		clproc->progressCallback = &progressCallback;
		if (auto cldiff = dynamic_cast<ClusterDiffComparer*>(clproc.get())) {
			cldiff->verbose = verbose;
			cldiff->printProgressDetails = verbose;
			cldiff->diffMap = &srcDiff;
		}
		auto t1 = GetTickCount();
		clproc->process(srcSelect);
		std::cerr << "Time: " << (GetTickCount() - t1) << std::endl;
	}
	if (action == DdAction::Compare || action == DdAction::Rcw) {
		diffClusterCount = ((ClusterDiffComparer&)(*clproc)).stats.clustersDiffCount;
		std::cerr << "Diff clusters: " << diffClusterCount << std::endl;
	}
	clproc.reset();



	//Dirty clusters after comparison
	if (action == DdAction::Compare || action == DdAction::Rcw)
		clusterPrinter.print(srcDiff);

	//Dirty files after comparison
	if ((action == DdAction::Compare || action == DdAction::Rcw) && filenamePrinter.active()) {
		filenamePrinter.open();
		LCN diffClustersInFilesTotal = 0;
		for (auto& fi : filemap) {
			if (!fi.second.dirty) continue;
			size_t bitCount = 0;
			for (auto& run : fi.second.runList)
				bitCount += srcDiff.bitCount(run.offset, run.offset + run.length - 1);
			if (bitCount <= 0) continue;
			std::string filename = fi.second.filename;
			if (filename.empty())
				filename = std::string{ "#" }+std::to_string(fi.first);
			filenamePrinter.printOne(filename + "\t" + std::to_string(bitCount));
			diffClustersInFilesTotal += bitCount;
		}
		//Must match diffClusterCount
		std::cerr << "Clusters in diff files:" << diffClustersInFilesTotal << std::endl;
	}



	// Cleanup


	if (bReturnExitCode) {
		//In List and Compare modes, return 0 only when there are no differences
		if (action == DdAction::List && candidateClusterCount > 0)
			return 1;
		if (action == DdAction::Compare && diffClusterCount > 0)
			return 1;
	}

	return 0;
}

int main(int argc, char* argv[]) {
	try {
		main2(argc, argv);
	}
	catch (const std::exception& e) {
		std::cerr << e.what();
		return -1;
	}
}