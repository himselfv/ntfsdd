#include <windows.h>
#include <winioctl.h>
#include <iostream>
#include <vector>
#include <string>
#include "ntfs.h"
#include "ntfsutil.h"
#include "ntfsvolume.h"
#include "ntfsmft.h"
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


enum class DdAction : int { List, Verify, Copy, Rvw };
template<> struct EnumNames<DdAction> {
	typedef std::map<std::string, DdAction> Map;
	static const Map map() {
		static const Map m{
			{ "list", DdAction::List },
			{ "verify", DdAction::Verify },
			{ "copy", DdAction::Copy },
			{ "rvw", DdAction::Rvw },
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
			{ "all", DdMode::All },
			{ "bitmap", DdMode::Bitmap },
			{ "mft", DdMode::MFT },
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


void buildFileTable(Volume& vol, Mft& mft)
{
	auto totalSegments = vol.volumeData().MftValidDataLength.QuadPart / vol.volumeData().BytesPerFileRecordSegment;
	std::cout << "Reading MFT segments..." << std::endl;
	auto segment = mft.newSegmentBuf();
	for (auto idx = 0; idx < totalSegments; idx++) {
		if (idx % 1000 == 0) std::cout << idx << " / " << totalSegments << std::endl;
		mft.readSegmentByIndex(idx, (FILE_RECORD_SEGMENT_HEADER*)(segment.data()));
	}
}


/*
struct FileAccounting {
	bool isDirty = false;
	std::set<uint64_t> clusterList;
};

std::map<uint64_t, FileAccounting> ProcessMftComparison(HANDLE hSrc, HANDLE hDest, uint32_t clusterSize) {
	std::map<uint64_t, FileAccounting> mftMap;

	const size_t mftEntrySize = 1024; // Standard NTFS MFT record size
	std::vector<uint8_t> bufferSrc(mftEntrySize);
	std::vector<uint8_t> bufferDest(mftEntrySize);

	uint64_t currentEntryIndex = 0;
	DWORD bytesReadSrc, bytesReadDest;

	// In a real scenario, you would determine the total MFT size from the $MFT entry itself
	while (ReadFile(hSrc, bufferSrc.data(), mftEntrySize, &bytesReadSrc, nullptr) && bytesReadSrc == mftEntrySize) {

		auto* headerSrc = reinterpret_cast<MftEntryHeader*>(bufferSrc.data());
		if (headerSrc->magic != 0x454C4946) { // "FILE"
			currentEntryIndex++;
			continue;
		}

		// 1. Identify the Accounting Key
		uint64_t baseKey = (headerSrc->baseMftRecord == 0) ? currentEntryIndex : headerSrc->baseMftRecord;
		auto& record = mftMap[baseKey];

		// 2. Determine Dirty State via Bytewise Comparison
		bool isCurrentEntryDirty = false;
		// Check if hDest is large enough to contain this MFT entry
		if (ReadFile(hDest, bufferDest.data(), mftEntrySize, &bytesReadDest, nullptr) && bytesReadDest == mftEntrySize) {
			if (std::memcmp(bufferSrc.data(), bufferDest.data(), mftEntrySize) != 0) {
				isCurrentEntryDirty = true;
			}
		}
		else {
			// Destination is smaller or MFT hasn't grown there yet
			isCurrentEntryDirty = true;
		}

		if (isCurrentEntryDirty) {
			record.isDirty = true;
		}

		// 3. Parse Attributes for Non-Resident Clusters
		uint16_t attrOffset = headerSrc->firstAttrOffset;
		while (attrOffset < headerSrc->usedSize) {
			auto* attr = reinterpret_cast<const uint8_t*>(bufferSrc.data() + attrOffset);
			uint32_t type = *reinterpret_cast<const uint32_t*>(attr);

			if (type == 0xFFFFFFFF) break; // End of attributes

			uint8_t nonResidentFlag = *(attr + 8);
			uint32_t length = *reinterpret_cast<const uint32_t*>(attr + 4);

			if (nonResidentFlag != 0) { // If attribute is Non-Resident
				ParseRunList(attr, record.clusterList);
			}

			attrOffset += length;


			if (length == 0) break;
		}

		currentEntryIndex++;
	}

	return mftMap;
}
*/

void printMft(Volume& vol, HANDLE hMft, int cnt)
{
	auto segmentSize = vol.volumeData().BytesPerFileRecordSegment;
	DWORD bytesRead = 0;
	std::vector<char> segment;
	segment.resize(segmentSize);
	for (auto i = 0; i < cnt; i++) {
		if (!ReadFile(hMft, segment.data(), segmentSize, &bytesRead, nullptr))
			throwLastOsError("Reading MFT sector.");
		auto header = (FILE_RECORD_SEGMENT_HEADER*)(segment.data());
		std::cout << "Sector: " << i << " " << header->MultiSectorHeader.Signature << " " << header->Flags << " " << header->SequenceNumber << std::endl;
	}
}

int main2(int argc, char* argv[]) {
	CLI::App app{ "NTFS Rapid Delta dd", "ntfsdd" };

	std::string srcPath, destPath;
	DdAction action{ DdAction::Verify };
	DdMode mode{ DdMode::MFT };
	DdTrim trim{ (DdTrim)(-1) };
	bool bSafetyOverride = false;

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
	VolumeLock srcLock(src.h());
	std::cerr << "Locking dest..." << std::endl;
	VolumeLock dstLock(dest.h());
	/*
	Dismount after locking. Dismounting triggers various activity for 5-8 seconds which prevents locking
	as system services keep the volume in use.
	Note that if you re-run the app too soon after a succesfull lock+dismount, new lock will fail due to the dismount activity still going on.
	
	But we don't really need to dismount, as NTFS "treats locked volumes as dismounted".
	So we're already in a more powerful state.
	DWORD bytesReturned;
	std::cerr << "Dismounting dest..." << std::endl;
	if (!DeviceIoControl(dest.h(), FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &bytesReturned, NULL))
		throwLastOsError("FSCTL_DISMOUNT_VOLUME");
	*/

	// Open and scan MFT
//	auto srcMft = src.openMft();
//	std::cerr << srcMft << std::endl;
//	auto destMft = dest.openMft();

	src.loadMftStructure();
//	printMft(src, srcMft, 16);

	buildFileTable(src, src.mft);

	exit(-1);


	DWORD clusterSize = src.volumeData().BytesPerCluster;

	VOLUME_BITMAP_BUFFER* srcBitmap = src.getVolumeBitmap();
	VOLUME_BITMAP_BUFFER* dstBitmap = dest.getVolumeBitmap();

/*
For cluster iteration:

	for (LONGLONG i = 0; i < numClusters.QuadPart; i++) {
		// Find the bit in the byte array
		bool isAllocated = (bitmapBuffer->Buffer[i / 8] & (1 << (i % 8))) != 0;

		if (isAllocated) {
			clusters_used++;
			LARGE_INTEGER byteOffset;
			byteOffset.QuadPart = i * clusterSize;

			// Call your verify-write logic
			ReadVerifyWrite(hSrc, hDest, byteOffset, clusterSize);
		}
		else
			clusters_free++;
*/


	/*
	Alignment: Ensure your "blocks" for verification match the SSD's physical page size (usually 4KB or 16KB) or the NTFS cluster size (usually 4KB).
	Hashing: Instead of comparing raw buffers, you might find that reading the source and destination and comparing a fast hash (like XXHash) is more CPU-efficient before deciding to trigger a write.
	TRIM: If you are "cloning," remember that for an SSD, knowing a sector is empty (via $Bitmap) is just as important as knowing it's full. You should issue IOCTL_STORAGE_MANAGE_DATA_SET_ATTRIBUTES with the DeviceDsmAction_Trim flag for all clusters marked 0 in the bitmap.

	*/

	// Cleanup
	free(dstBitmap);
	free(srcBitmap);

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