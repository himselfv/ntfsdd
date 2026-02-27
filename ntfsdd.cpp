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


enum class DdAction : int { List, Compare, Copy, Rvw, VerifyBitmap };
template<> struct EnumNames<DdAction> {
	typedef std::map<std::string, DdAction> Map;
	static const Map map() {
		static const Map m{
			{ "list", DdAction::List },			//List candidate sectors
			{ "compare", DdAction::Compare },	//Compare candidate sectors
			{ "copy", DdAction::Copy },			//Copy all candidate sectors
			{ "rvw", DdAction::Rvw },			//Verify candidate sectors and copy the changed ones
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
	DdAction action{ DdAction::Compare };
	DdMode mode{ DdMode::MFT };
	DdTrim trim{ (DdTrim)(-1) };
	bool bSafetyOverride = false;
	bool bPrintDirtyFiles = false;

	app.add_option("source", srcPath, "Source device/file")->required();
	app.add_option("target", destPath, "Target device/file")->required();

	// Options with set validation
	app.add_option("--action", action, "Action to take")
		->type_name("list|verify|copy|rvw")
		->transform(CLI::CheckedTransformer(EnumNames<DdAction>::map(), CLI::ignore_case).description(""))
		->capture_default_str()
		;

	app.add_option("--mode", mode, "Method to use")
		->type_name("all|bitmap|mft")
		->transform(CLI::CheckedTransformer(EnumNames<DdMode>::map(), CLI::ignore_case).description(""))
		->capture_default_str()
		;

	app.add_option("--trim", trim, "Trim unused sectors")
		->type_name("none|changes|all")
		->transform(CLI::CheckedTransformer(EnumNames<DdTrim>::map(), CLI::ignore_case).description(""))
		->capture_default_str()
		;

	app.add_flag("--safety-override", bSafetyOverride, "Continue even if the destination does not seem like the clone of the source")
		->capture_default_str()
		;

	app.add_flag("--print-dirty-files", bPrintDirtyFiles, "Print details on the MFT segments that are deemed dirty.")
		->capture_default_str()
		;


	CLI11_PARSE(app, argc, argv);

	// Open Handles
	auto src = Volume2();
	src.open(srcPath, GENERIC_READ);

	DWORD dwDestMode = GENERIC_READ;
//	if (action == DdAction::Copy || action == DdAction::Rvw)
//		dwDestMode |= GENERIC_WRITE;
	auto dest = Volume2();
	dest.open(destPath, dwDestMode);

	std::cerr << src.h() << dest.h() << std::endl;

	compareVolumeParams(src, dest, bSafetyOverride);

	// 2. Prepare Target (Lock and Dismount)
	std::cerr << "Locking src..." << std::endl;
	VolumeLock srcLock(src);
	std::cerr << "Locking dest..." << std::endl;
	VolumeLock dstLock(dest);
	/*
	Dismount after locking. Dismounting triggers various activity for 5-8 seconds which prevents locking
	as system services keep the volume in use.
	Note that if you re-run the app too soon after a succesfull lock+dismount, new lock will fail due to the dismount activity still going on.
	
	But we don't really need to dismount, as NTFS "treats locked volumes as dismounted".
	So we're already in a more powerful state.
	DWORD bytesReturned;
	std::cerr << "Dismounting dest..." << std::endl;
	if (!dest.ioctl(FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &bytesReturned, NULL))
		throwLastOsError("FSCTL_DISMOUNT_VOLUME");
	*/

	// Open and scan MFT
	std::cout << "Loading MFT structures..." << std::endl;
	src.loadMftStructure();
	dest.loadMftStructure();

	std::cout << "Loading stored bitmap..." << std::endl;
	NtfsBitmapFile srcBitmap(&src, &src.mft);
	NtfsBitmapFile destBitmap(&src, &src.mft);

	std::cout << "Verifying MFT layouts..." << std::endl;
	verifyMftLayout(src, src.mft, srcBitmap.buf);
	verifyMftLayout(dest, dest.mft, nullptr);

	BitmapBuf srcUsed;
	CandidateClusterMap srcDiff;
	if (action == DdAction::VerifyBitmap) {
		std::cout << "Recalculating $Bitmap..." << std::endl;
		auto t1 = GetTickCount();
		rebuildVolumeBitmap(src, src.mft, &srcUsed);
		std::cout << (GetTickCount() - t1) << std::endl;
	}
	if (action == DdAction::Copy || action == DdAction::List || action == DdAction::Compare || action == DdAction::Rvw) {
		std::cout << "Building file table bitmaps..." << std::endl;
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
			std::cout << "Reading MFT segments..." << std::endl;
			MftDiff diff(src.mft, dest.mft);
			diff.printDirtyFiles = bPrintDirtyFiles;
			diff.scan();
			srcDiff = std::move(diff.srcDiff);
			std::cout << "Used segments: " << diff.stats.usedSegments << std::endl;
			std::cout << "Dirty segments: " << diff.stats.dirtySegments << std::endl;
			std::cout << "Multisegments: " << diff.stats.multiSegments << std::endl;
			break;
		}
		}
		std::cout << (GetTickCount() - t1) << std::endl;
	}

	//≈сли в результате VerifyBitmap или любого действи€ с mode==MFT расчитали карту кластеров, то сравниваем еЄ с $Bitmap.
	//Ќо в не€вных случа€х молчим, если всЄ в пор€дке.
	if (srcUsed.size > 0) {
		//”беждаемс€, что srcUsed действительно закрывает то же, что говорит $Bitmap.
		std::cout << "Verifying file table bitmap..." << std::endl;
		auto cmp = compareBitmaps(srcBitmap.buf, &srcUsed);
		if (!cmp)
			throw std::runtime_error(std::string{ "Manually constructed bitmap is not identical to the NTFS one!" });
	}

	if (action == DdAction::Copy || action == DdAction::List || action == DdAction::Compare || action == DdAction::Rvw) {
		int64_t candidateClusterCount = 0;
		for (auto& run : BitmapSpans(&srcDiff)) {
			candidateClusterCount +=run.length;
			if (action == DdAction::List)
				std::cout << run.offset << "-" << run.offset+run.length << std::endl;
		}
		std::cout << "Candidate cluster count: " << candidateClusterCount << std::endl;

		//Safety: ѕровер€ем, что наш получившийс€ список содержит все кластеры srcBitmap, уникальные дл€ него (т.е. перешедшие в состо€ние 1 с момента destBitmap)
		//If $Bitmap shows a block was turned from free to used, warn and copy it, as that should not happen if I'm parsing MFT correctly.
		auto t1 = GetTickCount();
		verifyDiffContainsNewClusters(srcDiff, srcBitmap.asBitmap().andNot(destBitmap.asBitmap()));
		std::cout << (GetTickCount() - t1) << std::endl;

		ClusterDiffComparer cldiff(src, dest);
		t1 = GetTickCount();
		cldiff.process(srcDiff);
		std::cout << (GetTickCount() - t1) << std::endl;
		std::cout << "Diff clusters: " << cldiff.stats.diffCount << std::endl;
	}


	// Cleanup
	std::cout << "Done.\n";
	return 0;
}

int main(int argc, char* argv[]) {
	try {
		main2(argc, argv);
	}
	catch (const std::exception& e) {
		std::cout << e.what();
		return -1;
	}
}