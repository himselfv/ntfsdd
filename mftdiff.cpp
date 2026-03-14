#pragma once
#include "mftdiff.h"
#include <unordered_map>
#include "util.h"


class FileNameAttributeReader {
	std::vector<wchar_t> filenameBuf;

public:
	void updateEntry(ATTRIBUTE_RECORD_HEADER& attr, FileEntry* fileEntry)
	{
		assert(attr.FormCode != NONRESIDENT_FORM);
		FILE_NAME* fndata = (FILE_NAME*)((char*)&attr + attr.Form.Resident.ValueOffset);
		if (fndata->Flags & FILE_NAME_NTFS || !fileEntry->filenameNtfs) {
			if (filenameBuf.size() < fndata->FileNameLength + 2)
				filenameBuf.resize(fndata->FileNameLength + 2);
			memcpy(filenameBuf.data(), fndata->FileName, fndata->FileNameLength * 2);
			filenameBuf[fndata->FileNameLength] = 0x00;
			fileEntry->filename = wcharToUtf8((wchar_t*)(filenameBuf.data()));
			if (fndata->Flags & FILE_NAME_NTFS) fileEntry->filenameNtfs = true;
		}
	}
};


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
	//Убеждаемся, что правый MFT по расположению на диске совпадает с левым или является его началом.
	auto& srcMap = mftSrc.vcnMap();
	auto& destMap = mftDest.vcnMap();
	assert(srcMap.size() >= destMap.size());
	for (size_t i = 0; i < destMap.size(); i++) {
		assert(srcMap[i].lcnStart == destMap[i].lcnStart);
		assert(srcMap[i].vcnStart == destMap[i].vcnStart);
		assert(srcMap[i].len == destMap[i].len);
	}
}

void MftDiff::scan()
{
	//Убеждаемся, что конфигурация диска одна и та же
	auto TotalClusters = mftSrc.vol->volumeData().TotalClusters.QuadPart;
	assert(TotalClusters == mftDest.vol->volumeData().TotalClusters.QuadPart);
	auto BytesPerFileRecordSegment = mftSrc.vol->volumeData().BytesPerFileRecordSegment;
	assert(BytesPerFileRecordSegment == mftDest.vol->volumeData().BytesPerFileRecordSegment);

	this->verifyMftRunsCompatible();

	srcUsed.resize(TotalClusters);
	srcUsed.clear_all();
	srcDiff.resize(TotalClusters);
	srcDiff.clear_all();

	FileEntry tempSegmentEntry{};
	FileNameAttributeReader filenameReader{};

	auto totalSegments = mftSrc.vol->volumeData().MftValidDataLength.QuadPart / mftSrc.vol->volumeData().BytesPerFileRecordSegment;
	SegmentNumber segmentNo = -1;

	if (this->progressCallback) {
		this->progressCallback->setMax(totalSegments);
		this->progressCallback->progress(0, true);
	}

	auto srcIter = SegmentIter(&mftSrc);
	auto srcIt = srcIter.begin();
	auto destIter = SegmentIter(&mftDest);
	auto destIt = destIter.begin();

	for (; srcIt != srcIter.end(); ++srcIt) {
		segmentNo++;
		if (segmentNo % 1000 == 0) this->onProgress(segmentNo, totalSegments);
		if (this->progressCallback)
			this->progressCallback->progress(segmentNo, false);

		bool dirty = false;
		if (segmentNo > 0) { //For segmentNo==0 we've already read destIter.begin().
					   //Достаём такой же сегмент из правого MFT
					   //Это надо сделать в любом случае, даже если левый сектор невалидный, т.к. мы должны идти нога в ногу.
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
		SegmentNumber baseSegmentNumber = (*srcIt).BaseFileRecordSegment.SegmentNumberLowPart + ((*srcIt).BaseFileRecordSegment.SegmentNumberHighPart << sizeof(ULONG));

		//SequenceNumber не особо важен в этих целях.
		//Можно было бы попытаться что-то сделать за одну итерацию атрибутов, но тогда пришлось бы сохранять отдельно все увиденные runs,
		//т.к. в любой момент может выйти, что их всё-таки надо было в *какой-то* записи регистрировать.
		//Пробежать лишний раз по атрибутам дешевле этих операций с памятью.
		if (!baseSegmentNumber)
			for (auto& attr : AttributeIterator(&(*srcIt)))
				if (attr.TypeCode == $ATTRIBUTE_LIST) {
					baseSegmentNumber = segmentNo;
					break;
				}


		//Обращаться к filemap нужно по следующим причинам:
		//1. Это часть мультисегмента (заглавная или дочерняя), тогда надо обработку пропустить, а мультисгемент дополнить.
		//2. Простой segment dirty, а нас попросили filemapListDirty.
		//3. Проверить, не задан для для этого сегмента принудительный skip или dirty.
		//Для третьего пункта обращаться придётся в любом случае.

		FileEntry* segmentEntry = nullptr;
		if (baseSegmentNumber != 0) {
			segmentEntry = &filemap[baseSegmentNumber];
			segmentEntry->multisegment = true;
			stats.multiSegments++;
		} else if (dirty && this->filemapListDirty) {
			segmentEntry = &filemap[segmentNo];
		} else {
			auto it = filemap.find(segmentNo);
			if (it == filemap.end()) {
				segmentEntry = &tempSegmentEntry;
				segmentEntry->reset();
			} else
				segmentEntry = &it->second;
		}

		if (dirty)
			segmentEntry->dirty = true;


		//Перебираем атрибуты.
		for (auto& attr : AttributeIterator(&(*srcIt))) {
			if (attr.TypeCode == $FILE_NAME && dirty && filemapNeedNames)
				filenameReader.updateEntry(attr, segmentEntry);
			if (attr.FormCode != NONRESIDENT_FORM) continue;
			for (auto& run : DataRunIterator(&attr, DRI_SKIP_SPARSE)) {
				//assert(run.offset >= 0 || (attr.Flags & ATTRIBUTE_FLAG_SPARSE)); //Sparse runs are supposed to only appear in sparse attributes!
				//But no, $BadClus:$Bad has sparse runs even without this flag.
				srcUsed.set(run.offset, run.offset + run.length - 1);
				segmentEntry->totalClusters += run.length;
				segmentEntry->runList.push_back(run);
			}
		}


		//Простые файлы отмечаем немедленно:
		if (dirty && !segmentEntry->multisegment)
			this->onDirtyFile(segmentNo, *segmentEntry);
	}

	//Теперь проходим filemap и выставляем все кластеры от файлов, у которых хотя бы один сегмент dirty.
	for (auto& pair : filemap)
		if (pair.second.dirty && pair.second.multisegment)
			this->onDirtyFile(pair.first, pair.second);
}

void MftDiff::onProgress(SegmentNumber idx, SegmentNumber totalSegments)
{
}

void MftDiff::onDirtyFile(const SegmentNumber segmentNo, const FileEntry& fi)
{
	if (fi.skip)
		return;

	for (auto& run : fi.runList)
		srcDiff.set(run);
}
