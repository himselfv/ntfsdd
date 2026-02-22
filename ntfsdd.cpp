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
			for (auto& run : DataRunIterator(&attr))
				bmp->set(run.offset, run.offset + run.length - 1);
		}
		if (idx % 1000 == 0) std::cerr << idx << " / " << totalSegments << std::endl;
		idx++;
	}
}


void compareBitmaps(const VOLUME_BITMAP_BUFFER* bmp1, const Bitmap* bmp2)
{
	if (bmp1->StartingLcn.QuadPart % (sizeof(int64_t) * 8) != 0)
		throw std::runtime_error("StartingLcn is not a multiple of a sufficiently beautiful number, I didn't expect that!");
	auto diff = memcmp(bmp1->Buffer, &bmp2->data[bmp1->StartingLcn.QuadPart / (sizeof(int64_t) * 8)], (bmp1->BitmapSize.QuadPart + 7) / 8);
	if (diff >= 0)
		throw std::runtime_error(std::string{ "A difference in the byte " } +std::to_string(diff) + " of our bitmaps!");
}


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

void fileTableDiff(Mft& mftSrc, Mft& mftDest, BitmapBuf& srcUsed, BitmapBuf& srcDiff)
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
		//Достаём такой же сегмент из правого MFT
		//Это надо сделать в любом случае, даже если левый сектор невалидный, т.к. мы должны идти нога в ногу.
		if (destIt != destIter.end())
			++destIt;
		if (destIt != destIter.end());
		else
			dirty = true;

		//Невалидные слева сегменты пропускаем
		//Даже если когда-то они ссылались на какие-то кластеры, всё это сводится к ситуации "кластеры больше не используются",
		//которая решается однократным выкидыванием всех вновь занулённых кластеров по сравнению двух битмапов.
		//Или ещё лучше, вообще нами не решается, а делается время от времени defrag /Retrim.
		//Все кластеры самих MFT *будут* скопированы - за это отвечает запись MFT. И поэтому список фактически используемых кластеров *станет* актуальным.
		if (!mftSrc.isValidSegment(&*srcIt)) continue;
		if (((*srcIt).Flags & FILE_RECORD_SEGMENT_IN_USE) == 0) continue;

		//Если итератор справа есть, то сравниваем кластеры.
		if (!dirty)
			dirty = (0 == memcmp(&(*srcIt), &(*destIt), BytesPerFileRecordSegment));

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
		}

		//Перебираем атрибуты.
		for (auto& attr : AttributeIterator(&(*srcIt))) {
			if (attr.FormCode != NONRESIDENT_FORM) continue;
			for (auto& run : DataRunIterator(&attr)) {
				srcUsed.set(run.offset, run.offset + run.length - 1);
				if (multiSegmentEntry != nullptr)
					multiSegmentEntry->runList.push_back(run);
				else if (dirty)
					srcDiff.set(run.offset, run.offset + run.length - 1);
			}
		}
	}

	//Теперь проходим filemap и выставляем все кластеры от файлов, у которых хотя бы один сегмент dirty.
	for (auto& pair : filemap)
		if (pair.second.dirty) {
			for (auto& run : pair.second.runList)
				srcDiff.set(run.offset, run.offset + run.length - 1);
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
	std::cout << "Loading MFT structures..." << std::endl;
	src.loadMftStructure();
	dest.loadMftStructure();

	std::cout << "Loading stored bitmap..." << std::endl;
	NtfsBitmapFile srcBitmap(&src, &src.mft);

	std::cout << "Verifying MFT layouts..." << std::endl;
	verifyMftLayout(src, src.mft, srcBitmap.buf);
	verifyMftLayout(dest, dest.mft, nullptr);

	/*
	std::cout << "Building file table..." << std::endl;
	auto t1 = GetTickCount();
	BitmapBuf srcUsed;
	rebuildVolumeBitmap(src, src.mft, &srcUsed);
	std::cout << (GetTickCount() - t1) << std::endl;
	*/

	std::cout << "Building file table bitmaps..." << std::endl;
	auto t1 = GetTickCount();
	BitmapBuf srcUsed;
	BitmapBuf srcDiff;
	fileTableDiff(src.mft, dest.mft, srcUsed, srcDiff);
	std::cout << (GetTickCount() - t1) << std::endl;

	//Убеждаемся, что srcUsed действительно закрывает то же, что говорит $Bitmap.
	std::cout << "Verifying file table bitmap..." << std::endl;
	compareBitmaps(srcBitmap.buf, &srcUsed);

	int64_t dirtySectorCount = 0;
	for (size_t idx = 0; idx < srcDiff.size; idx++)
		if (srcDiff.get(idx)) {
			dirtySectorCount++;
			if (action == DdAction::List)
				std::cout << idx << std::endl;
		}
	std::cout << "Dirty sector count: " << dirtySectorCount << std::endl;

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