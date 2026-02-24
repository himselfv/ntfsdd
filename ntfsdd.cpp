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
	//Проверяем, что MFT более-менее закрывает собой весь диск.
	auto& volData = vol.volumeData();

	//Размеры посекторно и покластерно могут различаться с точностью до большего из них
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
	for (auto& segment : ExclusiveSegmentIter(&mft)) {
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
	Медленная, но более подробная версия
	auto diff = Bitmap(srcBitmap.buf->Buffer, srcBitmap.buf->BitmapSize.QuadPart) ^ srcUsed;
	auto ret = diff.isZero();
	if (!ret)
		diff.print();
	return ret;
*/
}


/*
At this point it feels like keeping this as a vector of runs is more efficient than a bitmap.
We're normally going to have not a lot of these run candidates and we'll want to read in continuous chunks anyway.
std::map could keep us sorted, but we don't really need sorted.

We're going to make this a little bit compatible with Bitmap, but do not expect us to handle overlapping ClusterRuns.
These do not normally occur in our tasks.
*/
class ClusterRunList : public std::vector<ClusterRun> {
public:
	ClusterRunList() {
		//Reserve a lot of space for efficiency
		this->reserve(8192);
	}
	inline void set(const LCN offset, const LCN length) { this->emplace_back(offset, length); }
	inline void set(const ClusterRun& run) { this->push_back(run); }
};

//typedef ClusterRunList CandidateClusterMap;
typedef BitmapBuf CandidateClusterMap;

typedef std::vector<ClusterRun> RunList;

/*
Получает указатели на два MFT, source и dest. Возвращает две карты кластеров:
1. Все использованные кластеры по мнению первой MFT (мнение второй - устарело).
2. Все кластеры, которые необходимо проверить на изменения.

Проходит по первой MFT, отслеживая переключения между runs и номер текущего кластера.
Поддерживает параллельно итератор второй MFT, который должен находиться точно в том же месте.
Надо убедиться, что их список runs идентичен с точностью до возможных дополнительных runs с левой стороны.
Если это не так, операция прерывается и диски несовместимы.

Иначе двигается сегмент за сегментом, сравнивая их побайтово. Неидентичные сегменты считаются dirty.
Если dirty-сегмент отдельностоящий, сразу же вносим все его кластеры non-resident атрибутов в карту dirty.

Иначе, если у него есть baseSegment ИЛИ атрибут ATTRIBUTE_LIST, то регистрируем его кластеры в учётной записи,
связанной с его baseSegment (с ним самим, если он не имеет baseSegment, но содержит ATTRIBUTE_LIST).
Регистрируем их в любом случае, но если наш сегмент dirty, то взводим dirty всей записи.

После достижения правой стороной финиша (== end()) левая сторона продолжается до её end() заведомо в режиме dirty.

После завершения сканирования обеих сторон проходим сохранённые учётные записи и отмечаем все кластеры в тех из них,
которые помечены как dirty.
*/

struct FileEntry {
	bool dirty = false;
	std::vector<ClusterRun> runList;
};

struct DiffStats {
	SegmentNumber usedSegments = 0;
	SegmentNumber dirtySegments = 0;
	SegmentNumber multiSegments = 0;
};

struct DiffOptions {
	bool printDirtyFiles = false;
};

void fileTableDiff(Mft& mftSrc, Mft& mftDest, BitmapBuf& srcUsed, CandidateClusterMap& srcDiff, DiffStats* stats, const DiffOptions& options)
{
	std::unordered_map<int64_t, FileEntry> filemap;

	//Убеждаемся, что конфигурация диска одна и та же
	auto TotalClusters = mftSrc.vol->volumeData().TotalClusters.QuadPart;
	assert(TotalClusters == mftDest.vol->volumeData().TotalClusters.QuadPart);
	auto BytesPerFileRecordSegment = mftSrc.vol->volumeData().BytesPerFileRecordSegment;
	assert(BytesPerFileRecordSegment == mftDest.vol->volumeData().BytesPerFileRecordSegment);

	//Убеждаемся, что правый MFT по расположению на диске совпадает с левым или является его началом.
	{
		auto& srcMap = mftSrc.vcnMap();
		auto& destMap = mftDest.vcnMap();
		assert(srcMap.size() >= destMap.size());
		for (size_t i = 0; i < destMap.size(); i++) {
			assert(srcMap[i].lcnStart == destMap[i].lcnStart);
			assert(srcMap[i].vcnStart == destMap[i].vcnStart);
			assert(srcMap[i].len == destMap[i].len);
		}
	}


	srcUsed.resize(TotalClusters);
	srcUsed.clear_all();
	srcDiff.resize(TotalClusters);
	srcDiff.clear_all();
//	srcDiff.clear();

	std::vector<wchar_t> filenameBuf;
	using convert_type = std::codecvt_utf8<wchar_t>;
	std::wstring_convert<convert_type, wchar_t> converter;

	std::cout << "Reading MFT segments..." << std::endl;
	auto totalSegments = mftSrc.vol->volumeData().MftValidDataLength.QuadPart / mftSrc.vol->volumeData().BytesPerFileRecordSegment;
	SegmentNumber idx = -1;
	auto srcIter = ExclusiveSegmentIter(&mftSrc);
	auto destIter = ExclusiveSegmentIter(&mftDest);
	auto destIt = destIter.begin();
	for (auto srcIt = srcIter.begin(); srcIt != srcIter.end(); ++srcIt) {
		idx++;
		if (idx % 1000 == 0) std::cout << idx << " / " << totalSegments << std::endl;

		bool dirty = false;
		if (idx > 0) { //For idx==0 we've already read destIter.begin().
			//Достаём такой же сегмент из правого MFT
			//Это надо сделать в любом случае, даже если левый сектор невалидный, т.к. мы должны идти нога в ногу.
			if (destIt != destIter.end())
				++destIt;
			if (destIt != destIter.end());
			else
				dirty = true;
		}

		//Always mark the MFT itself as dirty. We must always check it, and its own entry doesn't always reflect changes.
		if (idx == 0)
			dirty = true;

		//Невалидные слева сегменты пропускаем
		//Даже если когда-то они ссылались на какие-то кластеры, всё это сводится к ситуации "кластеры больше не используются",
		//которая решается однократным выкидыванием всех вновь занулённых кластеров по сравнению двух битмапов.
		//Или ещё лучше, вообще нами не решается, а делается время от времени defrag /Retrim.
		//Все кластеры самих MFT *будут* скопированы - за это отвечает запись MFT. И поэтому список фактически используемых кластеров *станет* актуальным.
		if (!mftSrc.isValidSegment(&*srcIt)) continue;
		if (((*srcIt).Flags & FILE_RECORD_SEGMENT_IN_USE) == 0) continue;

		if (stats)
			stats->usedSegments++;

		//Если итератор справа есть, то сравниваем кластеры.
		if (!dirty) {
			/*
			Very often, the update sequence number will increase without anything else changing at all, to the files not touched in ages.
			I have no idea why this happens. None of the ideas suggested by me or Gemini makes sense.
			Probably will have to parse $UsnJrnl to find this out. Anyway, zero out this USHORT to avoid these false matches.
			NOTE: We still want to copy the updated MFT entry with the new magic number just in case! But not the clusters with the data for this file.
			*/
			*((USHORT*)((char*)srcIt.segment + srcIt.segment->MultiSectorHeader.UpdateSequenceArrayOffset)) = 0x0000;
			*((USHORT*)((char*)destIt.segment + destIt.segment->MultiSectorHeader.UpdateSequenceArrayOffset)) = 0x0000;
			dirty = (0 != memcmp(&(*srcIt), &(*destIt), BytesPerFileRecordSegment));
		}
		if (dirty && stats)
			stats->dirtySegments++;

		//Дальше выясняем, является ли этот сегмент особым.
		//Простые сегменты можно сразу же помечать по кластерам. Для особых нужно добавить их кластеры в базовую запись.
		//Выяснить, что кластер особый, иногда можно сразу (base_segment), а иногда только после перебора атрибутов.

		SegmentNumber baseSegmentNumber = (*srcIt).BaseFileRecordSegment.SegmentNumberLowPart + ((*srcIt).BaseFileRecordSegment.SegmentNumberHighPart << sizeof(ULONG));
		//SequenceNumber не особо важен в этих целях.
		//Можно было бы попытаться что-то сделать за одну итерацию атрибутов, но тогда пришлось бы сохранять отдельно все увиденные runs,
		//т.к. в любой момент может выйти, что их всё-таки надо было в *какой-то* записи регистрировать.
		//Пробежать лишний раз по атрибутам дешевле этих операций с памятью.
		if (!baseSegmentNumber)
			for (auto& attr : AttributeIterator(&(*srcIt)))
				if (attr.TypeCode == $ATTRIBUTE_LIST) {
					baseSegmentNumber = idx;
					break;
				}

		//Иначе перебираем атрибуты, но сначала добываем для мульти-записи её учётную запись.
		FileEntry* multiSegmentEntry = nullptr;
		if (baseSegmentNumber != 0) {
			multiSegmentEntry = &filemap[baseSegmentNumber];
			if (dirty) multiSegmentEntry->dirty = true;
			if (stats) stats->multiSegments++;
		}

		//Перебираем атрибуты.
		std::string filename {};
		bool filenameNtfs = false;
		LCN totalFileClusters = 0;
		for (auto& attr : AttributeIterator(&(*srcIt))) {
			if (attr.TypeCode == $FILE_NAME && dirty && options.printDirtyFiles) {
				assert(attr.FormCode != NONRESIDENT_FORM);
				FILE_NAME* fndata = (FILE_NAME*)((char*)&attr + attr.Form.Resident.ValueOffset);
				if (fndata->Flags & FILE_NAME_NTFS || !filenameNtfs) {
					if (filenameBuf.size() < fndata->FileNameLength + 2)
						filenameBuf.resize(fndata->FileNameLength + 2);
					memcpy(filenameBuf.data(), fndata->FileName, fndata->FileNameLength * 2);
					filenameBuf[fndata->FileNameLength] = 0x00;
					filename = converter.to_bytes((wchar_t*)(filenameBuf.data()));
					if (fndata->Flags & FILE_NAME_NTFS) filenameNtfs = true;
				}
			}
			if (attr.FormCode != NONRESIDENT_FORM) continue;
			for (auto& run : DataRunIterator(&attr, DRI_SKIP_SPARSE)) {
				//assert(run.offset >= 0 || (attr.Flags & ATTRIBUTE_FLAG_SPARSE)); //Sparse runs are supposed to only appear in sparse attributes!
				//But no, $BadClus:$Bad has sparse runs even without this flag.
				srcUsed.set(run.offset, run.offset + run.length - 1);
				totalFileClusters += run.length;
				if (multiSegmentEntry != nullptr)
					multiSegmentEntry->runList.push_back(run);
				else if (dirty)
					srcDiff.set(run);
			}
		}

		//Выводим информацию об изменившихся файлах, если нас попросили
		if (dirty && options.printDirtyFiles) {
			if (filename.empty())
				filename = std::string{ "#" }+std::to_string(idx);
			std::cout << "Dirty: " << filename << " clusters=" << totalFileClusters << std::endl;
		}
	}

	//Теперь проходим filemap и выставляем все кластеры от файлов, у которых хотя бы один сегмент dirty.
	for (auto& pair : filemap)
		if (pair.second.dirty) {
			for (auto& run : pair.second.runList)
				srcDiff.set(run);
		}
}


/*
Safety: Проверяем, что наша карта кластеров для проверки содержит, как минимум, все те кластеры, которые *добавились* в более новом битмапе.
*/
void verifyDiffContainsNewClusters(CandidateClusterMap& srcDiff, const BitmapBuf& newlyUsedClusters)
{
	//Поскольку srcDiff это сам по себе Bitmap, то и тут можем обойтись удобной массовой операцией.
	auto remainder = newlyUsedClusters.andNot(srcDiff);
	if (!remainder.isZero())
		throw std::runtime_error("Assertion failed: some of the newly used clusters are not covered by the newly constructed difference map!");
}



void runCompare(Volume& src, Volume& dest, CandidateClusterMap& srcDiff)
{
	//TODO: Read from two streams
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
			DiffStats stats;
			DiffOptions options;
			options.printDirtyFiles = bPrintDirtyFiles;
			fileTableDiff(src.mft, dest.mft, srcUsed, srcDiff, &stats, options);
			std::cout << "Used segments: " << stats.usedSegments << std::endl;
			std::cout << "Dirty segments: " << stats.dirtySegments << std::endl;
			std::cout << "Multisegments: " << stats.multiSegments << std::endl;
			break;
		}
		}
		std::cout << (GetTickCount() - t1) << std::endl;
	}

	//Если в результате VerifyBitmap или любого действия с mode==MFT расчитали карту кластеров, то сравниваем её с $Bitmap.
	//Но в неявных случаях молчим, если всё в порядке.
	if (srcUsed.size > 0) {
		//Убеждаемся, что srcUsed действительно закрывает то же, что говорит $Bitmap.
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

		//Safety: Проверяем, что наш получившийся список содержит все кластеры srcBitmap, уникальные для него (т.е. перешедшие в состояние 1 с момента destBitmap)
		verifyDiffContainsNewClusters(srcDiff, srcBitmap.asBitmap().andNot(destBitmap.asBitmap()));

		runCompare(src, dest, srcDiff);
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