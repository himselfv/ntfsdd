#pragma once
#include "mftdiff.h"
#include <unordered_map>
#include "util.h"

//#define MFTDIFF_EXTRA_CHECKS
//For when you're looking for the impossible

void FilenameMap::process(SegmentNumber segmentNo, ATTRIBUTE_RECORD_HEADER& attr) {
	auto& entry = (*this)[segmentNo];
	assert(attr.FormCode != NONRESIDENT_FORM);
	FILE_NAME* fndata = (FILE_NAME*)((char*)&attr + attr.Form.Resident.ValueOffset);
	if (fndata->Flags & FILE_NAME_NTFS || !entry.filenameNtfs) {
		entry.filename = wcharToUtf8(fndata->FileName, fndata->FileName+fndata->FileNameLength);
		if (fndata->Flags & FILE_NAME_NTFS) entry.filenameNtfs = true;
	}
	if (entry.parentDir == -1 && fndata->ParentDirectory.mergedValue != 0)
		entry.parentDir = fndata->ParentDirectory.segmentNumber();
}

std::string FilenameMap::getFullPath(SegmentNumber segmentNo)
{
	std::string result {};
	while (segmentNo >= 0) {
		auto& entry = (*this)[segmentNo];
		std::string filename {};
		if (entry.filename.empty())
			filename = std::string{ "#" } +std::to_string(segmentNo);
		else
			filename = entry.filename;
		result = filename + (!result.empty() ? std::string{ "\\" } +result : std::string{});
		if (entry.parentDir == segmentNo) //Root dir does this
			segmentNo = -1;
		else
			segmentNo = entry.parentDir;
	}
	return result;
}


void DiffStats::print(int BytesPerCluster)
{
	qInfo() << "Segments: used=" << usedSegments << ", dirty=" << dirtySegments << std::endl;
	qVerbose() << "Multisegments: " << multiSegments << std::endl;
	if (filesSkipped != 0 || clustersSkipped != 0)
		qInfo() << "Skipped: files=" << filesSkipped << ", clusters=" << clustersSkipped
		<< ", size=" << dataSizeToStr(clustersSkipped*BytesPerCluster) << std::endl;
	qVerbose() << "Dirty bc: diff=" << dirtyBecauseOfCmp << ", index=" << dirtyBecauseOfIndex << ", parent=" << dirtyBecauseOfParent << std::endl;
}


MftScan::MftScan(Mft& mftSrc)
	: mftSrc(mftSrc)
{
}


void MftScan::scanInit()
{
	TotalClusters = mftSrc.vol->volumeData().TotalClusters.QuadPart;
	BytesPerFileRecordSegment = mftSrc.vol->volumeData().BytesPerFileRecordSegment;

	srcUsed.resize(TotalClusters);
	srcUsed.clear_all();

	totalSegments = mftSrc.vol->volumeData().MftValidDataLength.QuadPart / mftSrc.vol->volumeData().BytesPerFileRecordSegment;
	if (this->filenames)
		this->filenames->resize(totalSegments);

	if (this->progressCallback) {
		this->progressCallback->setMax(totalSegments);
		this->progressCallback->progress(0, true);
	}
}

void MftScan::scan()
{
	this->scanInit();

	FileEntry tempSegmentEntry{};
	SegmentNumber segmentNo = -1;

	auto srcIter = SegmentIter(&mftSrc);
	auto srcIt = srcIter.begin();

	for (; srcIt != srcIter.end(); ++srcIt) {
		segmentNo++;
		if (this->progressCallback)
			this->progressCallback->progress(segmentNo, false);

		if (!mftSrc.isValidSegment(&*srcIt))
			continue;
		if ((srcIt->Flags & FILE_RECORD_SEGMENT_IN_USE) == 0) continue;

		//0 is a valid segment number but sequenceNumber==0 is reserved so segment:sequence==0:0 safely indicates "not set".
		SegmentNumber baseSegmentNumber = -1; //We use -1 for the same here
		if (srcIt->BaseFileRecordSegment.mergedValue != 0) //If segment:sequence together is 0:0
			baseSegmentNumber = srcIt->BaseFileRecordSegment.segmentNumber();

		if (baseSegmentNumber < 0)
			for (auto& attr : AttributeIterator(&(*srcIt)))
				if (attr.TypeCode == $ATTRIBUTE_LIST) {
					baseSegmentNumber = segmentNo;
					break;
				}


		//Find appropriate permanent or temporary segmentEntry for this segment
		FileEntry* segmentEntry = nullptr;
		if (baseSegmentNumber >= 0) {
			segmentEntry = &filemap[baseSegmentNumber];
			assert(segmentEntry->multisegment || segmentEntry->totalClusters == 0); //Either it's new or already multisegment
			segmentEntry->multisegment = true;
		}
		else {
			segmentEntry = &filemap[segmentNo];
		}

		//Process the attributes
		this->processAttributes(segmentNo, segmentEntry, &(*srcIt));
	}
}

//Разбирает интересующие нас атрибуты сегмента segment в FileEntry* segmentEntry.
void MftScan::processAttributes(SegmentNumber segmentNo, FileEntry* segmentEntry, FILE_RECORD_SEGMENT_HEADER* segment)
{
	for (auto& attr : AttributeIterator(segment)) {
		if (attr.TypeCode == $FILE_NAME && filenames != nullptr)
			filenames->process(segmentNo, attr);
		if (attr.FormCode != NONRESIDENT_FORM) continue;
		if (attr.TypeCode == $INDEX_ALLOCATION) segmentEntry->hasIndexAllocations = true;
		for (auto& run : DataRunIterator(&attr, DRI_SKIP_SPARSE)) {
			assert(run.offset >= 0); //We asked the iterator to skip sparse runs
			//assert(run.offset >= 0 || (attr.Flags & ATTRIBUTE_FLAG_SPARSE)); //Sparse runs are supposed to only appear in sparse attributes!
			//But no, $BadClus:$Bad has sparse runs even without this flag.

			//Verify that no two files reference the same clusters:
			assert(srcUsed.bitCount(run.offset, run.offset + run.length - 1) == 0);
			//Mark clusters as used - always, even when skipping the segment processing later
			srcUsed.set(run.offset, run.offset + run.length - 1);
#ifdef MFTDIFF_EXTRA_CHECKS
			//Verify that we can set bits and count them:
			assert_eq(srcUsed.bitCount(run.offset, run.offset + run.length - 1), run.length);
#endif
			//Store all runs in the segment entry, we'll decide later if we mark them
			segmentEntry->totalClusters += run.length;
			segmentEntry->runList.push_back(run);
		}
	}
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
MftDiff::MftDiff(Mft& mftSrc, Mft& mftDest)
	: MftScan(mftSrc), mftDest(mftDest)
{
	/*
	Some files can change without any indication in the MFT.
	I have seen this happen to:
	  0 MFT
	  2 LogFile
	  6 Bitmap (perhaps)
      32, 33 $TxfLog-related
	It might be that not all of them are needed, but let's play it safe and add all of them:
	*/
	for (int i = 0; i < 33; i++)
		filemap[i].dirty = true;
	//If some are not used, nbd

	/*
	Other such files don't have preallocated numbers. The ones I've observed:
		System Volume Information\$CBT2
	The ones I've read about:
		$Extend\$UsnJrnl

	At the moment I have only a partial system to handle this:
	We will add segmentNumbers of some dirs and all files under these dirs will be marked dirty.
	This covers $Extend, but not System Volume Information (which is dynamic).
	*/
	for (int i = 0; i < 33; i++)
		this->dirtySubtreeRoots.insert(i);
}

void MftDiff::skipSegments(const std::unordered_set<SegmentNumber>& segments)
{
	for (auto& segNo : segments)
		this->filemap[segNo].skip = true;
}

void MftDiff::addDirtySegments(const std::unordered_set<SegmentNumber>& segments)
{
	for (auto& segNo : segments)
		this->filemap[segNo].dirty = true;
}

void MftDiff::addDirtySubtreeRoots(const std::unordered_set<SegmentNumber>& segments)
{
	this->dirtySubtreeRoots.insert(segments.begin(), segments.end());
}


void MftDiff::verifyMftRunsCompatible()
{
	//Require that the left-side MFT layout matches the right-side or extends it. MFTs never shrink.
	auto& srcMap = mftSrc.vcnMap();
	auto& destMap = mftDest.vcnMap();
	assert(srcMap.size() >= destMap.size());
	for (size_t i = 0; i < destMap.size(); i++) {
		assert(srcMap[i].lcnStart == destMap[i].lcnStart);
		assert(srcMap[i].vcnStart == destMap[i].vcnStart);
		assert(srcMap[i].len == destMap[i].len);
	}
}

void MftDiff::scanInit()
{
	MftScan::scanInit();

	//Убеждаемся, что конфигурация диска одна и та же
	assert(TotalClusters == mftDest.vol->volumeData().TotalClusters.QuadPart);
	assert(BytesPerFileRecordSegment == mftDest.vol->volumeData().BytesPerFileRecordSegment);

	this->verifyMftRunsCompatible();

	srcDiff.resize(TotalClusters);
	srcDiff.clear_all();
}


void MftDiff::scan()
{
	this->scanInit();

	FileEntry tempSegmentEntry{};

	SegmentNumber segmentNo = -1;

	auto srcIter = SegmentIter(&mftSrc);
	auto srcIt = srcIter.begin();
	auto destIter = SegmentIter(&mftDest);
	auto destIt = destIter.begin();

#ifdef MFTDIFF_EXTRA_CHECKS
	auto test = &*srcIt;
	test = nullptr;
#endif

	for (; srcIt != srcIter.end(); ++srcIt) {
#ifdef MFTDIFF_EXTRA_CHECKS
		assert(test != &*srcIt); // verify progression
		test = &*srcIt;
#endif
		segmentNo++;
		if (this->progressCallback)
			this->progressCallback->progress(segmentNo, false);

		bool dirty = false;
		if (segmentNo > 0) { //For segmentNo==0 we've already read destIter.begin().
			//Extract this exact segment from the right-side MFT.
			//This has to be done even if the left side is invalid as two sides have to march in lockstep.
			if (destIt != destIter.end())
				++destIt;
			if (destIt != destIter.end());
			else
				dirty = true;
		}

		//Невалидные слева сегменты пропускаем
		//Даже если когда-то они ссылались на какие-то кластеры, всё это сводится к ситуации "кластеры больше не используются",
		//которая решается однократным выкидыванием всех вновь занулённых кластеров по сравнению двух битмапов.
		//Или ещё лучше, вообще нами не решается, а делается время от времени defrag /Retrim.
		//Все кластеры самих MFT *будут* скопированы - за это отвечает запись MFT. И поэтому список фактически используемых кластеров *станет* актуальным.
		if (!mftSrc.isValidSegment(&*srcIt)) {
			continue;
		}
		if ((srcIt->Flags & FILE_RECORD_SEGMENT_IN_USE) == 0) continue;

		stats.usedSegments++;

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
			if (dirty)
				stats.dirtyBecauseOfCmp++;
		}


		//Дальше выясняем, является ли этот сегмент особым.
		//Простые сегменты можно сразу же помечать по кластерам. Для особых нужно добавить их кластеры в базовую запись.
		//Выяснить, что кластер особый, иногда можно сразу (base_segment), а иногда только после перебора атрибутов.

		//0 is a valid segment number but sequenceNumber==0 is reserved so segment:sequence==0:0 safely indicates "not set".
		SegmentNumber baseSegmentNumber = -1; //We use -1 for the same here
		if (srcIt->BaseFileRecordSegment.mergedValue != 0) //If segment:sequence together is 0:0
			baseSegmentNumber = srcIt->BaseFileRecordSegment.segmentNumber();

		//SequenceNumber не особо важен в этих целях.
		//Можно было бы попытаться что-то сделать за одну итерацию атрибутов, но тогда пришлось бы сохранять отдельно все увиденные runs,
		//т.к. в любой момент может выйти, что их всё-таки надо было в *какой-то* записи регистрировать.
		//Пробежать лишний раз по атрибутам дешевле этих операций с памятью.
		//Кроме того, нам нужно предварительно собрать и ещё кое-какую информацию.
		if (baseSegmentNumber < 0)
			for (auto& attr : AttributeIterator(&(*srcIt)))
				if (attr.TypeCode == $ATTRIBUTE_LIST) {
					baseSegmentNumber = segmentNo;
				}
				else if (attr.TypeCode == $INDEX_ALLOCATION && this->markAllIndexClustersDirty && !dirty) {
					dirty = true;
					stats.dirtyBecauseOfIndex++;
				}
				else if (attr.TypeCode == $FILE_NAME && !dirty) {
					//Any time a file is included in a dir, it gets another $FILE_NAME with backreference to that dir
					//Any backreference to a dirtySegmentRoot means the segment should be marked dirty.
					AttrFilename attrFn{ &attr };
					if (attrFn.fn->ParentDirectory.classic.SequenceNumber != 0
						&& dirtySubtreeRoots.find(attrFn.fn->ParentDirectory.segmentNumber()) != dirtySubtreeRoots.end())
					{
						dirty = true;
						stats.dirtyBecauseOfParent++;
					}
				}


		//By this point we should have determined whether this local segment is dirty
		if (dirty)
			stats.dirtySegments++;


		//Reasons to access filemap:
		//1. This is a part of multisegment (base or extension) => skip the processing and append to multisegment.
		//2. Plain segment is dirty and we need to filemapListDirty.
		//3. To check if this segment is pre-configured for skip or dirty.
		//Third option requires us to check anyways.

		//Find appropriate permanent or temporary segmentEntry for this segment
		FileEntry* segmentEntry = nullptr;
		if (baseSegmentNumber >= 0) {
			segmentEntry = &filemap[baseSegmentNumber];
			assert(segmentEntry->multisegment || segmentEntry->totalClusters == 0); //Either it's new or already multisegment
			segmentEntry->multisegment = true;
			stats.multiSegments++;
		} else if (dirty && this->filemapListDirty) {
			segmentEntry = &filemap[segmentNo];
		} else {
			auto it = filemap.find(segmentNo);
			if (it == filemap.end()) {
				segmentEntry = &tempSegmentEntry;
				segmentEntry->reset();
			} else {
				segmentEntry = &it->second;
				//If this entry has been marked dirty ahead of time, respect that and mark its clusters dirty.
				if (segmentEntry->dirty)
					dirty = true;
			}
		}

		//Mark the dirtiness down in the temporary, permanent standalone or permanent multisegment entry.
		if (dirty)
			segmentEntry->dirty = true;

		//Process the attributes
		this->processAttributes(segmentNo, segmentEntry, &(*srcIt));

		//Mark single-segment files immediately
		if (dirty && !segmentEntry->multisegment)
			this->onDirtyFile(segmentNo, *segmentEntry);
	}



	//Теперь проходим filemap и выставляем все кластеры от файлов, у которых хотя бы один сегмент dirty.
	for (auto& pair : filemap)
		if (pair.second.dirty && pair.second.multisegment) {
			this->onDirtyFile(pair.first, pair.second);
		}

#ifdef MFTDIFF_EXTRA_CHECKS
	size_t totalDirtyClustersAgain = 0;
	for (auto& pair : filemap)
		if (pair.second.dirty) {
			totalDirtyClustersAgain += pair.second.totalClusters;
		}
	qDebug() << "Total dirty clusters mk2: " << totalDirtyClustersAgain << std::endl;
#endif
}

void MftDiff::onDirtyFile(const SegmentNumber segmentNo, const FileEntry& fi)
{
	if (fi.skip) {
		stats.filesSkipped++;
		stats.clustersSkipped += fi.totalClusters;
		return;
	}

	for (auto& run : fi.runList)
		srcDiff.set(run);
}
