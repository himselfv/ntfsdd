#include <windows.h>
#include <winioctl.h>
#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include "ntfs.h"
#include "ntfsutil.h"
#include "ntfsvolume.h"
#include "ntfsmft.h"
#include "bitmap.h"
#include "mftdiff.h"
#include "clusterdiff.h"
#include <CLI/CLI.hpp>



template<typename Enum>
struct EnumNames {};
template<typename Enum, decltype(EnumNames<Enum>::map)* names = nullptr>
inline
std::string enumName(Enum value) {
	for (auto& pair : EnumNames<Enum>::map())
		if (pair.second == value) return pair.first;
	return std::string{};
}


enum class DdAction : int { List, Compare, Copy, Rcw, VerifyBitmap };
template<> struct EnumNames<DdAction> {
	typedef std::map<std::string, DdAction> Map;
	static const Map map() {
		static const Map m{
			{ "list", DdAction::List },			//List candidate sectors
			{ "compare", DdAction::Compare },	//Compare candidate sectors and print the differences
			{ "copy", DdAction::Copy },			//Copy all candidate sectors
			{ "rcw", DdAction::Rcw },			//Compare candidate sectors and copy the changed ones
			{ "verifyBitmap", DdAction::VerifyBitmap },	//Rebuild $Bitmap from MFT and compare to the actual one.
		};
		return m;
	}
};
std::ostream& operator<<(std::ostream &os, const DdAction &value) {
	return (os << enumName(value));
}

enum class DdMode : int { All, Bitmap, MFT };
template<> struct EnumNames<DdMode> {
	typedef std::map<std::string, DdMode> Map;
	static const Map map() {
		const Map m{
			{ "all", DdMode::All },			//All sectors
			{ "bitmap", DdMode::Bitmap },	//All sectors in use at source according to $Bitmap
			{ "mft", DdMode::MFT },			//All sectors in use at source according to MFT segments that differ from destination
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
	typedef std::map<std::string, DdTrim> Map;
	static const Map map() {
		const Map m{
			{ "none", DdTrim::None },
			{ "changes", DdTrim::Changes },
			{ "all", DdTrim::All },
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

	virtual void open(const std::string& path, DWORD dwOpenMode)
	{
		Volume::open(path, dwOpenMode);
	}

	virtual void close()
	{
		Volume::close();
	}

	void loadMftStructure() {
		mft.loadMftStructure(this->volumeData().MftStartLcn.QuadPart);
	}

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
	//ѕровер€ем, что MFT более-менее закрывает собой весь диск.
	auto& volData = vol.volumeData();

	//–азмеры посекторно и покластерно могут различатьс€ с точностью до большего из них
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
	ћедленна€, но более подробна€ верси€
	auto diff = Bitmap(srcBitmap.buf->Buffer, srcBitmap.buf->BitmapSize.QuadPart) ^ srcUsed;
	auto ret = diff.isZero();
	if (!ret)
		diff.print();
	return ret;
*/
}


/*
Safety: ѕровер€ем, что наша карта кластеров дл€ проверки содержит, как минимум, все те кластеры, которые *добавились* в более новом битмапе.
*/
void verifyDiffContainsNewClusters(CandidateClusterMap& srcDiff, const BitmapBuf& newlyUsedClusters)
{
	//ѕоскольку srcDiff это сам по себе Bitmap, то и тут можем обойтись удобной массовой операцией.
	auto remainder = newlyUsedClusters.andNot(srcDiff);
	if (!remainder.isZero())
		throw std::runtime_error("Assertion failed: some of the newly used clusters are not covered by the newly constructed difference map!");
}



int main2(int argc, char* argv[]) {
	CLI::App app{ "NTFS Rapid Delta dd", "ntfsdd" };

	std::string srcPath, destPath;
	app.add_option("source", srcPath, "Source device/file")->required();
	app.add_option("target", destPath, "Target device/file")->required();

	DdAction action{ DdAction::Compare };
	app.add_option("--action", action, "Action to take")
		->type_name("list|verify|copy|rcw")
		->transform(CLI::CheckedTransformer(EnumNames<DdAction>::map(), CLI::ignore_case).description(""))
		->capture_default_str()
		;

	DdMode mode{ DdMode::MFT };
	app.add_option("--mode", mode, "Method to use")
		->type_name("all|bitmap|mft")
		->transform(CLI::CheckedTransformer(EnumNames<DdMode>::map(), CLI::ignore_case).description(""))
		->capture_default_str()
		;

/*
	Do you really want us messing with trim? No, you don't. Do defrag /retrim from time to time.

	DdTrim trim{ (DdTrim)(-1) };
	app.add_option("--trim", trim, "Trim unused sectors")
		->type_name("none|changes|all")
		->transform(CLI::CheckedTransformer(EnumNames<DdTrim>::map(), CLI::ignore_case).description(""))
		->capture_default_str()
		;
*/


	//We'll enforce FSCTL_LOCK_VOLUME where it seems reasonable and TRY it elsewhere.
	//These flags force us to insist on it even if we're not sure it should work.
	bool bForceLockSrc = false;
	bool bForceLockDest = false;
	app.add_flag("--force-lock-src", bForceLockSrc, "Force FSCTL_LOCK_VOLUME on the source even when it doesn't seem to be a volume.")
		->capture_default_str()
		;
	app.add_flag("--force-lock-dest", bForceLockDest, "Force FSCTL_LOCK_VOLUME on the destination even when it doesn't seem to be a volume.")
		->capture_default_str()
		;

	bool bAllowMounted = false;
	app.add_flag("--unsafe-allow-mounted", bAllowMounted, "Allow the destination volume to have drive letters and mount points. It is advised to never use this switch. Only write to the volumes that you have manually dismounted, to prevent accidental overwriting of unintended volumes.")
		->capture_default_str()
		;

	bool bAllowNonMatching = false;
	app.add_flag("--unsafe-allow-non-matching", bAllowNonMatching, "Continue even if the destination does not seem like the clone of the source. You will likely wreak havoc and destroy unrelated volume by mistake, unless in a very well understood special case.")
		->capture_default_str()
		;

	bool write_mode = false;
	app.add_flag("--write", write_mode, "Write to destination. Without this flag we will not actually open the destination as writeable.")
		->capture_default_str()
		;


	bool bPrintClustersAsRuns = true;
	app.add_flag("--clusters-as-runs", bPrintClustersAsRuns, "For modes that print cluster lists, print cluster runs instead of individual clusters.")
		->capture_default_str()
		;

	bool bPrintDirtyFiles = false;
	app.add_flag("--print-dirty-files", bPrintDirtyFiles, "Print details on the MFT segments that are deemed dirty.")
		->capture_default_str()
		;


	bool verbose = false;
	app.add_flag("--verbose", verbose, "Detailed logging.")
		->capture_default_str()
		;

/*
	Not implemented yet.

	std::vector<SegmentNumber> skipSegments {};
	app.add_option("--skip-segments", skipSegments, "Skip MFT entries with these numbers");
*/

	CLI11_PARSE(app, argc, argv);

	// Open Handles
	bool srcIsVolume = verifyMountPoints(srcPath, verbose, false);
	auto src = Volume2();
	src.open(srcPath, GENERIC_READ);

	bool destIsVolume = verifyMountPoints(destPath, verbose, !bAllowMounted);
	DWORD dwDestMode = GENERIC_READ;
//	if (write_mode && (action == DdAction::Copy || action == DdAction::Rcw))
//		dwDestMode |= GENERIC_WRITE;
	auto dest = Volume2();
	dest.open(destPath, dwDestMode);

	if (verbose) {
		std::cerr << "Source handle: " << src.h() << std::endl;
		std::cerr << "Destination handle: " << dest.h() << std::endl;
	}

	compareVolumeParams(src, dest, bAllowNonMatching);

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
	src.loadMftStructure();
	dest.loadMftStructure();

	std::cerr << "Loading stored bitmap..." << std::endl;
	NtfsBitmapFile srcBitmap(&src, &src.mft);
	NtfsBitmapFile destBitmap(&src, &src.mft);

	std::cerr << "Verifying MFT layouts..." << std::endl;
	verifyMftLayout(src, src.mft, srcBitmap.buf);
	verifyMftLayout(dest, dest.mft, nullptr);

	BitmapBuf srcUsed;
	CandidateClusterMap srcDiff;
	if (action == DdAction::VerifyBitmap) {
		std::cerr << "Recalculating $Bitmap..." << std::endl;
		auto t1 = GetTickCount();
		rebuildVolumeBitmap(src, src.mft, &srcUsed);
		std::cerr << (GetTickCount() - t1) << std::endl;
	}
	if (action == DdAction::Copy || action == DdAction::List || action == DdAction::Compare || action == DdAction::Rcw) {
		std::cerr << "Building file table bitmaps..." << std::endl;
		auto t1 = GetTickCount();
		switch (mode) {
		case DdMode::All:
			srcDiff.set(ClusterRun{ 0, src.volumeData().TotalClusters.QuadPart });
			break;
		case DdMode::Bitmap:
			for (auto& run : BitmapSpans((uint64_t*)srcBitmap.buf->Buffer, srcBitmap.buf->BitmapSize.QuadPart))
				srcDiff.set(run);
			break;
		case DdMode::MFT: {
			std::cerr << "Reading MFT segments..." << std::endl;
			MftDiff diff(src.mft, dest.mft);
			diff.printDirtyFiles = bPrintDirtyFiles;
			diff.scan();
			srcDiff = std::move(diff.srcDiff);
			std::cerr << "Used segments: " << diff.stats.usedSegments << std::endl;
			std::cerr << "Dirty segments: " << diff.stats.dirtySegments << std::endl;
			std::cerr << "Multisegments: " << diff.stats.multiSegments << std::endl;
			break;
		}
		}
		std::cerr << (GetTickCount() - t1) << std::endl;
	}

	//≈сли в результате VerifyBitmap или любого действи€ с mode==MFT расчитали карту кластеров, то сравниваем еЄ с $Bitmap.
	//Ќо в не€вных случа€х молчим, если всЄ в пор€дке.
	if (srcUsed.size > 0) {
		//”беждаемс€, что srcUsed действительно закрывает то же, что говорит $Bitmap.
		std::cerr << "Verifying file table bitmap..." << std::endl;
		auto cmp = compareBitmaps(srcBitmap.buf, &srcUsed);
		if (!cmp)
			throw std::runtime_error(std::string{ "Manually constructed bitmap is not identical to the NTFS one!" });
	}

	if (action == DdAction::Copy || action == DdAction::List || action == DdAction::Compare || action == DdAction::Rcw) {
		int64_t candidateClusterCount = 0;
		for (auto& run : BitmapSpans(&srcDiff)) {
			candidateClusterCount +=run.length;
			if (action == DdAction::List)
				printClusterSpan(run.offset, run.length, bPrintClustersAsRuns);
		}
		std::cerr << "Candidate cluster count: " << candidateClusterCount << std::endl;

		//Safety: ѕровер€ем, что наш получившийс€ список содержит все кластеры srcBitmap, уникальные дл€ него (т.е. перешедшие в состо€ние 1 с момента destBitmap)
		//If $Bitmap shows a block was turned from free to used, warn and copy it, as that should not happen if I'm parsing MFT correctly.
		auto t1 = GetTickCount();
		verifyDiffContainsNewClusters(srcDiff, srcBitmap.asBitmap().andNot(destBitmap.asBitmap()));
		std::cerr << (GetTickCount() - t1) << std::endl;
	}

	if (action == DdAction::Copy) {
		//TODO
	}
	else if (action == DdAction::Compare || action == DdAction::Rcw) {
		std::unique_ptr<ClusterDiffComparer> cldiff;
		if (action == DdAction::Rcw)
			cldiff.reset(new ClusterDiffWriter(src, dest));
		else
			cldiff.reset(new ClusterDiffComparer(src, dest));
		cldiff->printDiff = (action == DdAction::Compare);
		cldiff->printClustersAsSpans = bPrintClustersAsRuns;
		auto t1 = GetTickCount();
		cldiff->process(srcDiff);
		std::cerr << (GetTickCount() - t1) << std::endl;
		std::cerr << "Diff clusters: " << cldiff->stats.diffCount << std::endl;
	}


	// Cleanup
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