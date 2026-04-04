#pragma once
#include "mftdiff.h"
#include <unordered_map>
#include "util.h"

//#define MFTDIFF_EXTRA_CHECKS
//For when you're looking for the impossible


void ScanStats::print(int BytesPerCluster)
{
	qInfo() << "Segments: used=" << usedSegments;
	qVerbose() << ", multisegments=" << multiSegments;
	qInfo() << std::endl;
}

void DiffStats::print(int BytesPerCluster)
{
	qInfo() << "Segments: dirty=" << dirtySegments;
	qVerbose() << " bc: diff=" << dirtyBecauseOfCmp << ", usn=" << dirtyBecauseOfUsnOnly << ", index=" << dirtyBecauseOfIndex << ", parent=" << dirtyBecauseOfParent;
	qVerbose() << "; usn_diff=" << diffUsnOnly;
	qInfo() << std::endl;
	if (filesSkipped != 0 || clustersSkipped != 0)
		qInfo() << "Skipped: files=" << filesSkipped << ", clusters=" << clustersSkipped
		<< ", size=" << dataSizeToStr(clustersSkipped*BytesPerCluster) << std::endl;
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


/*
Scan core.
Keep the current state in the member fields so that component functions can access it.
*/
void MftScan::scan()
{
	this->scanInit();

	auto& segmentNo = this->segmentNo;
	segmentNo = -1;

	auto srcIter = SegmentIter(&mftSrc);
	this->srcIt = std::move(srcIter.begin());
	auto srcEnd = std::move(srcIter.end());

#ifdef MFTDIFF_EXTRA_CHECKS
	auto test = &*srcIt;
	test = nullptr;
#endif

	for (; srcIt != srcEnd; ++srcIt) {
#ifdef MFTDIFF_EXTRA_CHECKS
		assert(test != &*srcIt); // verify progression
		test = &*srcIt;
#endif
		segmentNo++;
		if (this->progressCallback)
			this->progressCallback->progress(segmentNo, false);

		this->processAnySegment();

		//Skip invalid segments entirely
		//Even if on the dest they reference any clusters, those are "clusters not used anymore",
		//handled by a single trimming of all unused clusters from the bitmaps comparison.
		//Or better yet, left to occasional defrag /Retrim.
		//Clusters of the MFT itself *will* be copied - the $MFT entry governs them. So the list of the used clusters *will* be up to date.
		if (!mftSrc.IsValidSegment(&*srcIt)) {
			continue;
		}
		if ((srcIt->Flags & FILE_RECORD_SEGMENT_IN_USE) == 0) continue;

		scanStats.usedSegments++;

		this->processValidSegment();

	}
}

//Called for every MFT segment, valid or not.
void MftScan::processAnySegment()
{
}

//Called for valid MFT segments which are IN_USE
void MftScan::processValidSegment()
{
	//0 is a valid segment number but sequenceNumber==0 is reserved so segment:sequence==0:0 safely indicates "not set".
	baseSegmentNumber = -1; //We use -1 for the same here
	if (srcIt->BaseFileRecordSegment.mergedValue != 0) //If segment:sequence together is 0:0
		baseSegmentNumber = srcIt->BaseFileRecordSegment.segmentNumber();

	if (baseSegmentNumber < 0)
		for (auto& attr : AttributeIterator(srcIt.segment))
			if (attr.TypeCode == $ATTRIBUTE_LIST) {
				baseSegmentNumber = segmentNo;
				break;
			}


	//Select permanent or temporary entry
	auto segmentEntry = this->selectSegmentEntry();

	//Process the attributes
	this->processAttributes(segmentNo, segmentEntry, srcIt.segment);

}


/*
Find appropriate permanent or temporary segmentEntry for the current segment.

Permanent records are always associated with multi-segment files.
Otherwise it depends on the flags.
*/
FileEntry* MftScan::selectSegmentEntry()
{
	/*
	Reasons to access filemap:
	1. This is a part of multisegment (base or extension) => skip the processing and append to multisegment.
	2. We've been asked to track all files.

	In a Differ descendant:
	3. Plain segment is dirty and we need to filemapListDirty.
	4. To check if this segment is pre-configured for skip or dirty.
	Final option requires us to check anyways.

	We COULD have skipped the check in this base scanner but it can be reused if we don't.
	Whatever is the reason this level or the levels below pushed a permanent entry, if it exists we have to use it.
	*/
	FileEntry* segmentEntry = nullptr;
	if (baseSegmentNumber >= 0) {
		segmentEntry = &filemap[baseSegmentNumber];
		assert(segmentEntry->multisegment || segmentEntry->totalClusters == 0); //Either it's new or already multisegment
		segmentEntry->multisegment = true;
		scanStats.multiSegments++;
	}
	else if (this->filemapListAll) {
		segmentEntry = &filemap[segmentNo];
	}
	else {
		auto it = filemap.find(segmentNo);
		if (it == filemap.end()) {
			segmentEntry = &tempSegmentEntry;
			segmentEntry->reset();
		}
		else
			segmentEntry = &it->second;
	}
	return segmentEntry;
}


//Разбирает интересующие нас атрибуты сегмента segment в FileEntry* segmentEntry.
void MftScan::processAttributes(SegmentNumber segmentNo, FileEntry* segmentEntry, FILE_RECORD_SEGMENT_HEADER* segment)
{
	for (auto& attr : AttributeIterator(segment)) {
		if (attr.TypeCode == $FILE_NAME && filenames != nullptr)
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

	//Verify that the volume params are identical
	assert(TotalClusters == mftDest.vol->volumeData().TotalClusters.QuadPart);
	assert(BytesPerFileRecordSegment == mftDest.vol->volumeData().BytesPerFileRecordSegment);

	this->verifyMftRunsCompatible();

	auto destIter = SegmentIter(&mftDest);
	this->destIt = destIter.begin();
	this->destEnd = destIter.end();

	srcDiff.resize(TotalClusters);
	srcDiff.clear_all();
}

void MftDiff::scan()
{
	MftScan::scan();

	//On completion, scan filemap and mark all clusters for the dirty multisegments
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

void MftDiff::processAnySegment()
{
	//Extract this exact segment from the right-side MFT.
	//This has to be done even if the left side is invalid as two sides have to march in lockstep.
	if (segmentNo > 0) { //For segmentNo==0 we've already read destIter.begin().
		if (destIt != destEnd)
			++destIt;
	}
}

void MftDiff::processValidSegment()
{
	auto& dirty = segmentDirty;
	dirty = false;

	//If we have a segment on the right, compare the data:
	if (destIt != destEnd) {
		/*
		Very often, the update sequence number will increase without anything else changing at all, to the files not touched in ages.
		I have no idea why this happens. None of the ideas suggested by me or Gemini makes sense.
		Probably will have to parse $UsnJrnl to find this out. Anyway, zero out this USHORT to avoid these false matches.
		NOTE: We still want to copy the updated MFT entry with the new magic number just in case! But not the clusters with the data for this file.
		*/
		//We're comparing this after segment processing so the only place where this sequence number is preserved is in the fixup array.
		auto srcUsnAddr = ((USHORT*)((char*)srcIt.segment + srcIt->MultiSectorHeader.UpdateSequenceArrayOffset));
		auto destUsnAddr = ((USHORT*)((char*)destIt.segment + destIt->MultiSectorHeader.UpdateSequenceArrayOffset));
		auto destUsnOrig = *destUsnAddr;
		*destUsnAddr = *srcUsnAddr;

		dirty = (0 != memcmp(&(*srcIt), &(*destIt), BytesPerFileRecordSegment));
		if (dirty)
			diffStats.dirtyBecauseOfCmp++;
		
		if (*srcUsnAddr != destUsnOrig && !dirty) {
			if (!this->ignoreUsnChanges) {
				dirty = true;
				diffStats.dirtyBecauseOfUsnOnly++;
			}
			diffStats.diffUsnOnly++;
		}
	} else
		dirty = true;


	//Simple segments can be marked on the map immediately. Multisegments have to be delayed until the end of the scan.
	//Multisegments are those with either base_segment set or $ATTRIBUTE_LIST in their attributes.

	//0 is a valid segment number but sequenceNumber==0 is reserved so segment:sequence==0:0 safely indicates "not set".
	baseSegmentNumber = -1; //We use -1 for the same here
	if (srcIt->BaseFileRecordSegment.mergedValue != 0) //If segment:sequence together is 0:0
		baseSegmentNumber = srcIt->BaseFileRecordSegment.segmentNumber();

	//SequenceNumber не особо важен в этих целях.
	//Можно было бы попытаться что-то сделать за одну итерацию атрибутов, но тогда пришлось бы сохранять отдельно все увиденные runs,
	//т.к. в любой момент может выйти, что их всё-таки надо было в *какой-то* записи регистрировать.
	//Пробежать лишний раз по атрибутам дешевле этих операций с памятью.
	//Кроме того, нам нужно предварительно собрать и ещё кое-какую информацию.
	if (baseSegmentNumber < 0)
		for (auto& attr : AttributeIterator(srcIt.segment))
			if (attr.TypeCode == $ATTRIBUTE_LIST) {
				baseSegmentNumber = segmentNo;
			}
			else if (attr.TypeCode == $INDEX_ALLOCATION && this->markAllIndexClustersDirty && !dirty) {
				dirty = true;
				diffStats.dirtyBecauseOfIndex++;
			}
			else if (attr.TypeCode == $FILE_NAME && !dirty) {
				//Any time a file is included in a dir, it gets another $FILE_NAME with backreference to that dir
				//Any backreference to a dirtySegmentRoot means the segment should be marked dirty.
				AttrFilename attrFn{ &attr };
				if (attrFn.fn->ParentDirectory.classic.SequenceNumber != 0
					&& dirtySubtreeRoots.find(attrFn.fn->ParentDirectory.segmentNumber()) != dirtySubtreeRoots.end())
				{
					dirty = true;
					diffStats.dirtyBecauseOfParent++;
				}
			}


	//By this point we should have determined whether this local segment is dirty
	if (dirty)
		diffStats.dirtySegments++;


	//Find appropriate permanent or temporary entry for this segment
	FileEntry* segmentEntry = this->selectSegmentEntry();

	//Process the attributes
	this->processAttributes(segmentNo, segmentEntry, srcIt.segment);

	//Mark single-segment files immediately
	if (dirty && !segmentEntry->multisegment)
		this->onDirtyFile(segmentNo, *segmentEntry);
}


FileEntry* MftDiff::selectSegmentEntry()
{
	auto& dirty = segmentDirty;
	FileEntry* segmentEntry = nullptr;

	//Force-route multisegments to inherited before doing our special cases, because they need a *different* segmentNumber cell + complex processing
	if (baseSegmentNumber >= 0)
		segmentEntry = MftScan::selectSegmentEntry();
	else if (dirty && this->filemapListDirty) {
		segmentEntry = &filemap[segmentNo];
	}
	else
		segmentEntry = MftScan::selectSegmentEntry();

	//If this entry has been marked dirty ahead of time, respect that and mark its clusters dirty.
	//For multisegment entries this will also expand their cumulative dirtyness to local dirtyness which is fine.
	if (segmentEntry->dirty)
		dirty = true;

	//Mark the dirtiness down in the temporary, permanent standalone or permanent multisegment entry.
	if (dirty)
		segmentEntry->dirty = true;

	return segmentEntry;
}



void MftDiff::onDirtyFile(const SegmentNumber segmentNo, const FileEntry& fi)
{
	if (fi.skip) {
		diffStats.filesSkipped++;
		diffStats.clustersSkipped += fi.totalClusters;
		return;
	}

	for (auto& run : fi.runList)
		srcDiff.set(run);
}
