#pragma once
#include "mftdiff.h"
#include <unordered_map>
#include <codecvt>


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

}

void MftDiff::scan()
{
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

	std::vector<wchar_t> filenameBuf;
	using convert_type = std::codecvt_utf8<wchar_t>;
	std::wstring_convert<convert_type, wchar_t> converter;

	auto totalSegments = mftSrc.vol->volumeData().MftValidDataLength.QuadPart / mftSrc.vol->volumeData().BytesPerFileRecordSegment;
	SegmentNumber idx = -1;
	auto srcIter = SegmentIter(&mftSrc);
	auto srcIt = srcIter.begin();
	auto destIter = SegmentIter(&mftDest);
	auto destIt = destIter.begin();

	for (; srcIt != srcIter.end(); ++srcIt) {
		idx++;
		if (idx % 1000 == 0) this->onProgress(idx, totalSegments);

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
					baseSegmentNumber = idx;
					break;
				}

		//Иначе перебираем атрибуты, но сначала добываем для мульти-записи её учётную запись.
		FileEntry* multiSegmentEntry = nullptr;
		if (baseSegmentNumber != 0) {
			multiSegmentEntry = &filemap[baseSegmentNumber];
			if (dirty) multiSegmentEntry->dirty = true;
			stats.multiSegments++;
		}

		//Перебираем атрибуты.
		ShortFileInfo finfo;
		bool filenameNtfs = false;
		for (auto& attr : AttributeIterator(&(*srcIt))) {
			if (attr.TypeCode == $FILE_NAME && dirty && printDirtyFiles) {
				assert(attr.FormCode != NONRESIDENT_FORM);
				FILE_NAME* fndata = (FILE_NAME*)((char*)&attr + attr.Form.Resident.ValueOffset);
				if (fndata->Flags & FILE_NAME_NTFS || !filenameNtfs) {
					if (filenameBuf.size() < fndata->FileNameLength + 2)
						filenameBuf.resize(fndata->FileNameLength + 2);
					memcpy(filenameBuf.data(), fndata->FileName, fndata->FileNameLength * 2);
					filenameBuf[fndata->FileNameLength] = 0x00;
					finfo.filename = converter.to_bytes((wchar_t*)(filenameBuf.data()));
					if (fndata->Flags & FILE_NAME_NTFS) filenameNtfs = true;
				}
			}
			if (attr.FormCode != NONRESIDENT_FORM) continue;
			for (auto& run : DataRunIterator(&attr, DRI_SKIP_SPARSE)) {
				//assert(run.offset >= 0 || (attr.Flags & ATTRIBUTE_FLAG_SPARSE)); //Sparse runs are supposed to only appear in sparse attributes!
				//But no, $BadClus:$Bad has sparse runs even without this flag.
				srcUsed.set(run.offset, run.offset + run.length - 1);
				finfo.totalClusters += run.length;
				if (multiSegmentEntry != nullptr)
					multiSegmentEntry->runList.push_back(run);
				else if (dirty)
					srcDiff.set(run);
			}
		}

		//Выводим информацию об изменившихся файлах, если нас попросили
		if (dirty && printDirtyFiles) {
			if (finfo.filename.empty())
				finfo.filename = std::string{ "#" }+std::to_string(idx);
		}
	}

	//Теперь проходим filemap и выставляем все кластеры от файлов, у которых хотя бы один сегмент dirty.
	for (auto& pair : filemap)
		if (pair.second.dirty) {
			for (auto& run : pair.second.runList)
				srcDiff.set(run);
		}
}

void MftDiff::onProgress(SegmentNumber idx, SegmentNumber totalSegments)
{
	std::cerr << idx << " / " << totalSegments << std::endl;
}

void MftDiff::onDirtyFile(const ShortFileInfo& fi)
{
	std::cerr << "Dirty: " << fi.filename << " clusters=" << fi.totalClusters << std::endl;
}
