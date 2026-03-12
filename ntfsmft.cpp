#pragma once
#include <algorithm>
#include "ntfsmft.h"
#include "mftutil.h"

LCN NonResidentData::getLcn(VCN vcn) {
	for (auto& span : this->m_vcnMap)
		if (vcn < span.vcnStart)
			return (LCN)(-1);
		else if (vcn < span.vcnStart + span.len)
			return (span.lcnStart + vcn - span.vcnStart);
	return (LCN)(-1);
}
//Некоторые файлы могут быть sparse, но иногда хочется проверить, что дыр нет.
VCN NonResidentData::getFirstMissingVcn() {
	VCN vcn = 0;
	for (auto& span : this->m_vcnMap)
		if (span.vcnStart > vcn)
			return vcn;
		else
			vcn = span.vcnStart + span.len + 1;
	return (VCN)(-1);
}
void NonResidentData::addAttr(ATTRIBUTE_RECORD_HEADER* attr) {
	if (attr->Form.Nonresident.LowestVcn == 0) {
		//Lowest run map attribute stores totals
		this->dataHeader = *attr;
	}
	VCN vcnpos = attr->Form.Nonresident.LowestVcn;
	for (auto& run : DataRunIterator(attr, DRI_SKIP_SPARSE)) {
		m_vcnMap.push_back(VcnMapEntry{ vcnpos, run.offset, run.length });
		vcnpos += run.length;
	}
	//WARNING: We list all allocated clusters, as described by run maps, but actual precise file size can be less!
	//Mind it when reading!
	std::sort(m_vcnMap.begin(), m_vcnMap.end(), [](const VcnMapEntry& a, const VcnMapEntry& b) { return a.vcnStart < b.vcnStart; });
}

void NonResidentData::readAll(void* buf)
{
	auto BytesPerCluster = vol->volumeData().BytesPerCluster;
	auto ptr = ((uint8_t*)buf);
	for (auto& run : this->m_vcnMap) {
		DWORD bytesRead = 0;
		LARGE_INTEGER vrbn;
		vrbn.QuadPart = run.lcnStart*BytesPerCluster;
		OSCHECKBOOL(vol->setFilePointer(vrbn));
		auto readSz = run.len*BytesPerCluster;
		auto remSz = this->dataHeader.Form.Nonresident.AllocatedLength - run.vcnStart*BytesPerCluster;
		if (remSz < readSz)
			readSz = remSz;
		OSCHECKBOOL(vol->read(ptr, (DWORD)readSz, &bytesRead, nullptr));
		assert(bytesRead == readSz);
		ptr += bytesRead;
	}
}



Mft::Mft(Volume* volume)
	: NonResidentData(volume)
{
	vol = volume;
}


//Actual loading has to take place after the source Volume figures out its layout.
//Minimal loading allows us to read arbitrary segments in the first few clusters.
//Full loading does some additional integrity checks.
void Mft::loadMinimal()
{
	// Pre-calculate and verify some values
	assert(vol->volumeData().BytesPerFileRecordSegment % vol->volumeData().BytesPerSector == 0, "MFT segment size is not a multiple of logical sector size!");
	this->SectorsPerFileSegment = vol->volumeData().BytesPerFileRecordSegment / vol->volumeData().BytesPerSector;
	this->BytesPerFileSegment = vol->volumeData().BytesPerFileRecordSegment;
}

void Mft::load()
{
	this->loadMinimal();
	loadMftStructure(vol->volumeData().MftStartLcn.QuadPart);
}

void Mft::loadMftStructure(LCN lcnFirst)
{
	auto segment = newSegmentBuf();
	readSegmentLcn(lcnFirst, (FILE_RECORD_SEGMENT_HEADER*)segment.data());
	auto header = (FILE_RECORD_SEGMENT_HEADER*)(segment.data());

	//Read attributes
	ATTRIBUTE_RECORD_HEADER* attrData = nullptr;
	for (auto& attr : AttributeIterator(header)) {
		if (attr.TypeCode == $DATA) {
			assert(!attrData);
			attrData = &attr;
		}
		assert(attr.TypeCode != $ATTRIBUTE_LIST); //Не поддерживаем $ATTRIBUTE_LIST в MFT!
	}
	assert(attrData != nullptr);
	assert(attrData->FormCode == NONRESIDENT_FORM);
	assert(attrData->Form.Nonresident.LowestVcn == 0); //Поскольку $ATTRIBUTE_LIST не поддерживаем, то тут должен быть единственный атрибут, закрывающий весь VCN.
	this->addAttr(attrData);

	assert(this->m_vcnMap.size() > 0); //Должны быть элементы в MFT.
	assert(this->m_vcnMap.front().lcnStart == lcnFirst); //Первый LCN MFT должен совпадать с полученным.
	assert(this->getFirstMissingVcn() == (uint64_t)(-1)); //Не поддерживаем пробелы в MFT
	assert(this->sizeInBytes() % this->BytesPerFileSegment == 0); //Размер должен быть кратен сегменту.
}

std::vector<char> Mft::newSegmentBuf()
{
	std::vector<char> segment;
	segment.resize(BytesPerFileSegment);
	return segment;
}

void Mft::readSegmentByIndex(int64_t segmentIndex, FILE_RECORD_SEGMENT_HEADER* segment)
{
	auto BytesPerCluster = vol->volumeData().BytesPerCluster;

	VRBN vrbn;
	vrbn.QuadPart = segmentIndex * BytesPerFileSegment;
	VCN vcn = vrbn.QuadPart / BytesPerCluster;
	vrbn.QuadPart %= BytesPerCluster;

	auto lcn = this->getLcn(vcn);
	assert(lcn >= 0);
	vrbn.QuadPart += lcn * BytesPerCluster;

	return this->readSegmentsVrbn(vrbn, segment, 1);
}

//Читает ПЕРВЫЙ сегмент в указанном логическом кластере. Их там может быть несколько!
void Mft::readSegmentLcn(LCN lcn, FILE_RECORD_SEGMENT_HEADER* segment)
{
	VRBN vrOffset;
	vrOffset.QuadPart = lcn * vol->volumeData().BytesPerCluster;
	return this->readSegmentsVrbn(vrOffset, segment, 1);
}

void Mft::readSegmentsVrbn(VRBN vrbn, FILE_RECORD_SEGMENT_HEADER* segment, int count)
{
	OSCHECKBOOL(vol->setFilePointer(vrbn));
	return readSegmentsNoSeek(segment, count);
}

void Mft::readSegmentsNoSeek(FILE_RECORD_SEGMENT_HEADER* segment, int count, LPOVERLAPPED lpOverlapped)
{
	DWORD bytesRead = 0;
	OSCHECKBOOL(vol->read(segment, count*BytesPerFileSegment, &bytesRead, nullptr));
	assert(bytesRead == count*BytesPerFileSegment);
	this->processSegments(segment, count);
}


void Mft::processSegments(FILE_RECORD_SEGMENT_HEADER* segment, int count, int* validCount)
{
	while (count > 0) {
		//So apparently records can be uninitialized. These appear closer to the end of the MFT.
		bool isValidSegment = (*((uint32_t*)(&(segment->MultiSectorHeader.Signature))) == *((uint32_t*)"FILE"));

		//Apply fixups
		if (isValidSegment) {
			this->segmentApplyFixups(segment);
			if (validCount != nullptr)
				(*validCount)++;
		}

		segment = (FILE_RECORD_SEGMENT_HEADER*)((uint8_t*)segment + BytesPerFileSegment);
		count--;
	}
}

bool Mft::IsValidSegment(FILE_RECORD_SEGMENT_HEADER* segment)
{
	return (*((uint32_t*)(&(segment->MultiSectorHeader.Signature))) == *((uint32_t*)"FILE"));
}

void Mft::segmentApplyFixups(FILE_RECORD_SEGMENT_HEADER* segment)
{
	auto fixupCnt = segment->MultiSectorHeader.UpdateSequenceArraySize - 1; //1 additional cell is for the UpdateValueNumber
	assert(fixupCnt == this->SectorsPerFileSegment);

	auto fixup = (uint16_t*)((char*)segment + segment->MultiSectorHeader.UpdateSequenceArrayOffset);
	auto pos = (char*)segment + vol->volumeData().BytesPerSector - 2;
	auto magic = *(fixup++);
	while (fixupCnt > 0) {
		assert(*((uint16_t*)pos) == magic, "Invalid fixup in FILE segment.");
		*((uint16_t*)pos) = *fixup;
		pos += vol->volumeData().BytesPerSector;
		fixup++;
		fixupCnt--;
	}
}


NtfsBitmapFile::NtfsBitmapFile(Volume* vol, Mft* mft)
	: NonResidentData(vol)
{
	std::vector<uint8_t> buffer;
	buffer.resize(vol->volumeData().BytesPerFileRecordSegment);
	auto segment = (FILE_RECORD_SEGMENT_HEADER*)(buffer.data());
	mft->readSegmentByIndex(BIT_MAP_FILE_NUMBER, segment);
	auto attr = AttributeIterator::findFirstAttr(segment, $DATA);
	assert(attr != nullptr);
	this->addAttr(attr);

	assert(attr->FormCode == NONRESIDENT_FORM);
	auto allocSize = sizeof(VOLUME_BITMAP_BUFFER) + attr->Form.Nonresident.AllocatedLength;
	this->buf = (VOLUME_BITMAP_BUFFER*)malloc(allocSize); //Rounded up to cluster for ease of reading
	this->readAll(&(buf->Buffer[0]));

	//Fill data in our VOLUME_BITMAP_BUFFER for some compatibility
	this->buf->StartingLcn.QuadPart = 0;
	this->buf->BitmapSize.QuadPart = vol->volumeData().TotalClusters.QuadPart; //This can be slightly less

	//Either the cluster count matches or it's rounded up.
	assert(this->sizeInBytes() == (this->buf->BitmapSize.QuadPart + 7) / 8);
}

NtfsBitmapFile::~NtfsBitmapFile()
{
	if (this->buf != nullptr) {
		free(this->buf);
		this->buf = nullptr;
	}
}


SegmentIteratorBase::SegmentIteratorBase(Mft* mft, Flags flags)
	: mft(mft), flags(flags)
{
	if (mft == nullptr) return; //null-initialized end() iterator
}

int64_t SegmentIteratorBase::selectMaxChunkSize()
{
	if (mft == nullptr) return 4096;
	//Наш блок на чтение:
	//- Однозначно кратен сегменту
	//- Лучше, чтобы кратен кластеру, чтобы проще было решать вопросы чтения
	//- Лучше всего ориентироваться на физический размер сектора диска, но SSD часто говорят его неправильно,
	//  512, когда реальный размер 4096.
	auto batchSize = mft->vol->volumeData().BytesPerFileRecordSegment;
	//Сегменты могут быть больше одного кластера! Хоть это и редко делают на практике.
	if (mft->vol->volumeData().BytesPerCluster > batchSize)
		batchSize = mft->vol->volumeData().BytesPerCluster;
	if (mft->vol->extendedVolumeData().BytesPerPhysicalSector > batchSize)
		batchSize = mft->vol->extendedVolumeData().BytesPerPhysicalSector;
	//В любом случае выравниваем на размер сегмента
	auto remainder = batchSize % mft->vol->volumeData().BytesPerFileRecordSegment;
	if (remainder != 0)
		batchSize = (batchSize + mft->vol->volumeData().BytesPerFileRecordSegment) - remainder;
	//Умножаем!
	batchSize *= SEGMENTITERATOR_BATCHSIZE;
	return batchSize;
}

//Advances 1 times until eof() or reads current segment
void SegmentIteratorBase::advance()
{
	this->segment = nullptr; //Cannot advance
}

//Advances 0 or more times until eof() or the first segment matching the criteria
void SegmentIteratorBase::readCurrent()
{
	while (currentRun != nullptr) {
		while (true) {
			if (!segment)
				break; //Valid run but need a new segment
			if ((flags & SI_SKIP_INVALID) && (!mft->isValidSegment(segment)))
				break;
			if ((flags & SI_SKIP_NOT_IN_USE) && ((segment->Flags & FILE_RECORD_SEGMENT_IN_USE) == 0))
				break;
			return;
		}
		this->advance();
	}
}


SegmentIteratorBuffered::SegmentIteratorBuffered(Mft* mft, Flags flags)
	: SegmentIteratorBase(mft, flags)
{
	if (mft == nullptr) return; //null-initialized end() iterator

	buffer.resize(this->selectMaxChunkSize());

	remainingRuns = (int)mft->vcnMap().size();
	if (remainingRuns > 0) {
		currentRun = mft->vcnMap().data();
		remainingRuns--;
		this->openRun();
		this->readInt();
		this->readCurrent();
	}
}

void SegmentIteratorBuffered::advanceRun()
{
	if (remainingRuns <= 0) {
		currentRun = nullptr;
		remainingSegmentsInRun = 0;
		return;
	}
	remainingRuns--;
	currentRun++;
	this->openRun();
}

//Open currently selected run. It must have at least one segment.
void SegmentIteratorBuffered::openRun()
{
	auto bytesPerCluster = mft->vol->volumeData().BytesPerCluster;
	vrbn.QuadPart = bytesPerCluster * currentRun->lcnStart;
	if (remainingRuns <= 0) {
		remainingSegmentsInRun = mft->sizeInSegments();
		for (size_t i=0; i<mft->vcnMap().size()-1; i++)
			remainingSegmentsInRun -= ((bytesPerCluster * mft->vcnMap().at(i).len) / mft->BytesPerFileSegment);
		remainingSegmentsInRun--;
	} else
		remainingSegmentsInRun = ((bytesPerCluster * currentRun->len) / mft->BytesPerFileSegment) - 1;
	OSCHECKBOOL(mft->vol->setFilePointer(vrbn));

	assert(remainingBufferData == 0); //just in case
}

void SegmentIteratorBuffered::advance()
{
	if (remainingSegmentsInRun > 0) {
		remainingSegmentsInRun--;
		vrbn.QuadPart += mft->BytesPerFileSegment; //Отслеживаем всегда, т.к. нужно для нескольких механизмов сразу.
	}
	else {
		this->advanceRun();
		if (!this->currentRun) {
			segment = nullptr;
			return;
		}
	}

	this->readInt();
}

//Assumes currentRun is valid and there's at least one segment record left in it.
//Advances segment pointer into the current buffer if there's enough buffered data left, or requests a new chunk of data and points segment into that.
//On exit we have a valid segment pointer.
void SegmentIteratorBuffered::readInt()
{
	if (remainingBufferData >= mft->BytesPerFileSegment) {
		segment = (FILE_RECORD_SEGMENT_HEADER*)((uint8_t*)segment + mft->BytesPerFileSegment);
		remainingBufferData -= mft->BytesPerFileSegment;
		//Не проверяем, что remainingBufferData делится на BytesPerFileSegment без остатка! Это должно быть верно.
	}
	else {
		//Читаем новый участок, в пределах доступного в этом run
		SegmentNumber segmentCount = (int64_t)(buffer.size()) / mft->BytesPerFileSegment;
		if (segmentCount > remainingSegmentsInRun + 1) //1 уже вычтен перед вызовом этого чтения
			segmentCount = remainingSegmentsInRun + 1;
		segment = (FILE_RECORD_SEGMENT_HEADER*)(buffer.data());
#ifdef SEGMENTITERATOR_ZEROMEM
		memset(buffer.data(), 0, buffer.size());
#endif
		mft->readSegmentsVrbn(vrbn, segment, (int)segmentCount);
		remainingBufferData = (segmentCount - 1)*(mft->BytesPerFileSegment);
	}

#ifdef SEGMENTITERATOR_TRACKPOS
	lbn += mft->BytesPerFileSegment;
	vcn++;
#endif
}


SegmentIteratorOverlapped::SegmentIteratorOverlapped(Mft* mft, Flags flags)
	: SegmentIteratorBase(mft, flags)
{
	if (mft == nullptr) return; //null-initialized end() iterator

	this->reader = new AsyncFileReader(mft->vol->h(), SEGMENTITERATOR_QUEUEDEPTH, this->selectMaxChunkSize());

	remainingRuns = (int)mft->vcnMap().size();
	if (remainingRuns > 0) {
		currentRun = mft->vcnMap().data();
		remainingRuns--;
		this->openRun();
		this->readCurrent();
	}
}

SegmentIteratorOverlapped::~SegmentIteratorOverlapped()
{
	if (this->reader) {
		delete this->reader;
		this->reader = nullptr;
	}
}

//TODO: Nothing here handles zero-length runs.
void SegmentIteratorOverlapped::advanceRun()
{
	if (remainingRuns <= 0) {
		currentRun = nullptr;
		currentClusterInRun = 0;
		return;
	}
	remainingRuns--;
	currentRun++;
	this->openRun();
}

//Open currently selected run. It must have at least one segment.
void SegmentIteratorOverlapped::openRun()
{
	auto bytesPerCluster = mft->vol->volumeData().BytesPerCluster;
/*	if (remainingRuns <= 0) {
		remainingClustersInRun = mft->sizeInBytes() / mft->vol->volumeData().BytesPerCluster;
		for (size_t i = 0; i<mft->vcnMap().size() - 1; i++)
			remainingClustersInRun -= mft->vcnMap().at(i).len;
	}
	else*/
		currentClusterInRun = 0;
}

void SegmentIteratorOverlapped::advance()
{
	auto BytesPerCluster = this->mft->vol->volumeData().BytesPerCluster;

	//Часть 1. Запихиваем команды чтения в очередь
	while (currentRun != nullptr) {
		auto offset = currentRun->lcnStart + currentClusterInRun;
		auto len = currentRun->len - currentClusterInRun;
		if (len > SEGMENTITERATOR_BATCHSIZE)
			len = SEGMENTITERATOR_BATCHSIZE;
		if (!this->reader->try_push_back(offset*BytesPerCluster, (uint32_t)(len*BytesPerCluster)))
			break;
		currentClusterInRun += len;
		if (currentClusterInRun >= currentRun->len)
			this->advanceRun();
#ifdef SEGMENTITERATOR_TRACKPOS
		readClustersPushed += len;
		readsPushed++;
#endif
	}

	//Часть 2. Дочитываем до конца текущий буфер.
	if (remainingBufferData >= mft->BytesPerFileSegment) {
		segment = (FILE_RECORD_SEGMENT_HEADER*)((uint8_t*)segment + mft->BytesPerFileSegment);
		remainingBufferData -= mft->BytesPerFileSegment;
#ifdef SEGMENTITERATOR_TRACKPOS
		segmentAdvances++;
#endif
		//Не проверяем, что remainingBufferData делится на BytesPerFileSegment без остатка! Это должно быть верно.
		return;
	}
	assert(remainingBufferData == 0);

	//Часть 3. Дожидаемся и вытаскиваем следующий запрос на чтение.

	//Если у нас есть предыдущий буфер, то мы его достали из читалки и должны ей вернуть
	if (segment != nullptr) {
#ifdef SEGMENTITERATOR_TRACKPOS
		readsPopped++;
#endif
		reader->pop_front();
		segment = nullptr;
	}

	remainingBufferData = 0; //Going to cast it to 32 bit so zero it
	segment = (FILE_RECORD_SEGMENT_HEADER*)reader->finalize_front((uint32_t*)&remainingBufferData);
	if (segment == nullptr)
		return;
	assert(remainingBufferData % mft->BytesPerFileSegment == 0);
#ifdef SEGMENTITERATOR_TRACKPOS
	readsPulled++;
	readClustersPulled += remainingBufferData / mft->vol->volumeData().BytesPerCluster;
	readSegmentsPulled += remainingBufferData / mft->BytesPerFileSegment;
	segmentAdvances++;
#endif
	mft->processSegments(segment, (int)(remainingBufferData / mft->BytesPerFileSegment));
	remainingBufferData -= mft->BytesPerFileSegment; //Read this right now
}
