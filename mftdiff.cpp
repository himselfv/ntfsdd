#pragma once
#include "mftdiff.h"
#include <unordered_map>
#include "util.h"

void FilenameMap::process(SegmentNumber segmentNo, ATTRIBUTE_RECORD_HEADER& attr) {
	auto& entry = (*this)[segmentNo];
	assert(attr.FormCode != NONRESIDENT_FORM);
	FILE_NAME* fndata = (FILE_NAME*)((char*)&attr + attr.Form.Resident.ValueOffset);
	if (fndata->Flags & FILE_NAME_NTFS || !entry.filenameNtfs) {
		entry.filename = wcharToUtf8(fndata->FileName, fndata->FileName+fndata->FileNameLength-1);
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
	: mftSrc(mftSrc), mftDest(mftDest)
{
	//Always mark the MFT itself as dirty. We must always check it and its own entry doesn't always reflect changes.
	//The callers are free to override us of course.
	filemap[0].dirty = true;
}

void MftDiff::skipSegments(const std::unordered_set<SegmentNumber>& segments)
{
	for (auto& segNo : segments)
		this->filemap[segNo].skip = true;
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

//#define MFTDIFF_EXTRA_CHECKS
//For when you're looking for the impossible

void MftDiff::scan()
{
	//Убеждаемся, что конфигурация диска одна и та же
	auto TotalClusters = mftSrc.vol->volumeData().TotalClusters.QuadPart;
	assert(TotalClusters == mftDest.vol->volumeData().TotalClusters.QuadPart);
	auto BytesPerFileRecordSegment = mftSrc.vol->volumeData().BytesPerFileRecordSegment;
	assert(BytesPerFileRecordSegment == mftDest.vol->volumeData().BytesPerFileRecordSegment);

	this->verifyMftRunsCompatible();

#ifdef MFTDIFF_EXTRA_CHECKS
	LCN totalUsedClusters = 0;
	LCN totalDirtyClusters = 0;
#endif

	srcUsed.resize(TotalClusters);
	srcUsed.clear_all();
	srcDiff.resize(TotalClusters);
	srcDiff.clear_all();

	FileEntry tempSegmentEntry{};

	auto totalSegments = mftSrc.vol->volumeData().MftValidDataLength.QuadPart / mftSrc.vol->volumeData().BytesPerFileRecordSegment;
	SegmentNumber segmentNo = -1;

	if (this->filenames)
		this->filenames->resize(totalSegments);

	if (this->progressCallback) {
		this->progressCallback->setMax(totalSegments);
		this->progressCallback->progress(0, true);
	}

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
		if (segmentNo % 1000 == 0) this->onProgress(segmentNo, totalSegments);
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
		}
		if (dirty)
			stats.dirtySegments++;


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
		if (baseSegmentNumber < 0)
			for (auto& attr : AttributeIterator(&(*srcIt)))
				if (attr.TypeCode == $ATTRIBUTE_LIST) {
					baseSegmentNumber = segmentNo;
					break;
				}


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

		if (dirty)
			segmentEntry->dirty = true;


		//Process the attributes
		for (auto& attr : AttributeIterator(&(*srcIt))) {
			if (attr.TypeCode == $FILE_NAME && filenames!=nullptr)
				filenames->process(segmentNo, attr);
			if (attr.FormCode != NONRESIDENT_FORM) continue;
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
				//Count the clusters again
				totalUsedClusters += run.length;
				if (dirty && !segmentEntry->multisegment)
					totalDirtyClusters += run.length;
#endif
				//Store all runs in the segment entry, we'll decide later if we mark them
				segmentEntry->totalClusters += run.length;
				segmentEntry->runList.push_back(run);
			}
		}

		//Mark single-segment files immediately
		if (dirty && !segmentEntry->multisegment)
			this->onDirtyFile(segmentNo, *segmentEntry);
	}

	//Теперь проходим filemap и выставляем все кластеры от файлов, у которых хотя бы один сегмент dirty.
	for (auto& pair : filemap)
		if (pair.second.dirty && pair.second.multisegment) {
			this->onDirtyFile(pair.first, pair.second);
#ifdef MFTDIFF_EXTRA_CHECKS
			totalDirtyClusters += pair.second.totalClusters;
#endif
		}

#ifdef MFTDIFF_EXTRA_CHECKS
	qDebug() << "Total used clusters: " << totalUsedClusters << std::endl;
	qDebug() << "Total dirty clusters: " << totalDirtyClusters << std::endl;

	size_t totalDirtyClustersAgain = 0;
	for (auto& pair : filemap)
		if (pair.second.dirty) {
			totalDirtyClustersAgain += pair.second.totalClusters;
		}
	qDebug() << "Total dirty clusters mk2: " << totalDirtyClustersAgain << std::endl;
#endif
}

void MftDiff::onProgress(SegmentNumber idx, SegmentNumber totalSegments)
{
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
