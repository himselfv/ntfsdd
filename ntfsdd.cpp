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

void verifyMftLayout(Volume& vol, Mft& mft, const VOLUME_BITMAP_BUFFER* bitmap)
{
	//Проверяем, что MFT более-менее закрывает собой весь диск.
	auto& volData = vol.volumeData();

	//Размеры посекторно и покластерно могут различаться с точностью до большего из них
	int64_t totalBytes = volData.NumberSectors.QuadPart*volData.BytesPerSector;
	auto totalBytesDiff = totalBytes - volData.TotalClusters.QuadPart*volData.BytesPerCluster;
	assert(totalBytesDiff < ((volData.BytesPerCluster > volData.BytesPerSector) ? volData.BytesPerCluster : volData.BytesPerSector));

	assert(volData.TotalClusters.QuadPart == bitmap->StartingLcn.QuadPart + bitmap->BitmapSize.QuadPart);
}



struct Bitmap {
public:
	constexpr static size_t BLOCK_BITS = sizeof(uint64_t) * 8;
	uint64_t* data = nullptr;
	int64_t size = 0;
	Bitmap() {};
	Bitmap(void* data) : data(static_cast<uint64_t*>(data)) {};
	void set(size_t lo, size_t hi) {
		if (lo > hi) return;
		assert(hi < size);

		size_t start_word = lo / BLOCK_BITS;
		size_t end_word = hi / BLOCK_BITS;
		size_t start_bit = lo % BLOCK_BITS;
		size_t end_bit = hi % BLOCK_BITS;

		// The entire range is within a single 64-bit word
		if (start_word == end_word) {
			uint64_t mask = (~0ULL << start_bit) & (~0ULL >> (BLOCK_BITS-1 - end_bit));
			data[start_word] |= mask;
			return;
		}

		// Range spans multiple words
		data[start_word] |= (~0ULL << start_bit);
		if (end_word > start_word + 1)
			std::memset(&data[start_word + 1], 0xFF, (end_word - start_word - 1) * sizeof(uint64_t));
		data[end_word] |= (~0ULL >> (BLOCK_BITS-1 - end_bit));
	}
	void clear(size_t lo, size_t hi) {
		if (lo > hi) return;
		assert(hi < size);

		size_t start_word = lo / BLOCK_BITS;
		size_t end_word = hi / BLOCK_BITS;
		size_t start_bit = lo % BLOCK_BITS;
		size_t end_bit = hi % BLOCK_BITS;

		if (start_word == end_word) {
			uint64_t mask = (~0ULL << start_bit) & (~0ULL >> (BLOCK_BITS-1 - end_bit));
			data[start_word] &= ~mask;
			return;
		}

		// Range spans multiple words
		data[start_word] &= ~(~0ULL << start_bit);
		if (end_word > start_word + 1)
			std::memset(&data[start_word + 1], 0x00, (end_word - start_word - 1) * sizeof(uint64_t));
		data[end_word] &= ~(~0ULL >> (BLOCK_BITS-1 - end_bit));
	}
};
struct BitmapAuto : public Bitmap {
public:
	std::vector<uint8_t> buffer;
	BitmapAuto() {}
	BitmapAuto(size_t size) {
		this->resize(size);
	}
	void resize(size_t size) {
		buffer.resize((size + 7) / 8);
		this->data = static_cast<uint64_t*>((void*)(buffer.data()));
		this->size = buffer.size() * 8;
		this->clear_all();
	}
	void clear_all() {
		this->clear(0, buffer.size() * 8 - 1);
	}
};

struct FileEntry {
	bool dirty = false;
	std::set<uint64_t> clusterList;
};

std::map<int64_t, FileEntry> filemap;

struct FileTable {
	BitmapAuto bmp;
};

FileTable buildFileTable(Volume& vol, Mft& mft, int64_t totalClusters)
{
	FileTable ret;
	ret.bmp.resize(totalClusters);

	auto t1 = GetTickCount();
	auto totalSegments = vol.volumeData().MftValidDataLength.QuadPart / vol.volumeData().BytesPerFileRecordSegment;
	std::cout << "Reading MFT segments..." << std::endl;
	VCN idx = 0;
	for (auto& segment : ExclusiveSegmentIter(&mft)) {
		if (!mft.isValidSegment(&segment)) continue;
		if ((segment.Flags & FILE_RECORD_SEGMENT_IN_USE) == 0) continue;
		for (auto& attr : AttributeIterator(&segment)) {
			if (attr.FormCode != NONRESIDENT_FORM) continue;
			for (auto& run : DataRunIterator(&attr))
				ret.bmp.set(run.offset, run.offset + run.length - 1);
		}
		if (idx % 1000 == 0) std::cout << idx << " / " << totalSegments << std::endl;
		idx++;
	}
	std::cout << (GetTickCount() - t1) << std::endl;
	return ret;
}

void compareBitmaps(const VOLUME_BITMAP_BUFFER* bmp1, const Bitmap* bmp2)
{
	if (bmp1->StartingLcn.QuadPart % (sizeof(int64_t) * 8) != 0)
		throw std::runtime_error("StartingLcn is not a multiple of a sufficiently beautiful number, I didn't expect that!");
	auto diff = memcmp(bmp1->Buffer, &bmp2->data[bmp1->StartingLcn.QuadPart / (sizeof(int64_t) * 8)], (bmp1->BitmapSize.QuadPart + 7) / 8);
	if (diff >= 0)
		throw std::runtime_error(std::string{ "A difference in the byte " } +std::to_string(diff) + " of our bitmaps!");
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
	src.loadMftStructure();

	std::cout << "Loading stored bitmap..." << std::endl;
	NtfsBitmapFile srcBitmap(&src, &src.mft);

	std::cout << "Verifying MFT layout..." << std::endl;
	verifyMftLayout(src, src.mft, srcBitmap.buf);


	std::cout << "Building file table..." << std::endl;
	auto srcFt = buildFileTable(src, src.mft, src.volumeData().TotalClusters.QuadPart);

	std::cout << "Verifying file table bitmap..." << std::endl;
	compareBitmaps(srcBitmap.buf, &(srcFt.bmp));
	return 0;


	dest.loadMftStructure();
	VOLUME_BITMAP_BUFFER* destBitmap = dest.queryVolumeBitmap();
	verifyMftLayout(dest, dest.mft, destBitmap);



	DWORD clusterSize = src.volumeData().BytesPerCluster;

	/*
	Alignment: Ensure your "blocks" for verification match the SSD's physical page size (usually 4KB or 16KB) or the NTFS cluster size (usually 4KB).
	Hashing: Instead of comparing raw buffers, you might find that reading the source and destination and comparing a fast hash (like XXHash) is more CPU-efficient before deciding to trigger a write.
	TRIM: If you are "cloning," remember that for an SSD, knowing a sector is empty (via $Bitmap) is just as important as knowing it's full. You should issue IOCTL_STORAGE_MANAGE_DATA_SET_ATTRIBUTES with the DeviceDsmAction_Trim flag for all clusters marked 0 in the bitmap.

	*/

	// Cleanup
	free(destBitmap);
	//free(srcBitmap);

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