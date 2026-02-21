#pragma once
#include <algorithm>
#include "ntfsmft.h"

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
	for (auto& run : DataRunIterator(attr)) {
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
		OSCHECKBOOL(SetFilePointerEx(vol->h(), vrbn, nullptr, FILE_BEGIN));
		auto readSz = run.len*BytesPerCluster;
		auto remSz = this->dataHeader.Form.Nonresident.AllocatedLength - run.vcnStart*BytesPerCluster;
		if (remSz < readSz)
			readSz = remSz;
		OSCHECKBOOL(ReadFile(vol->h(), ptr, (DWORD)readSz, &bytesRead, nullptr));
		assert(bytesRead == readSz);
		ptr += bytesRead;
	}
}



Mft::Mft(Volume* volume)
	: NonResidentData(volume)
{
	vol = volume;
}

void Mft::load()
{
	loadMftStructure(vol->volumeData().MftStartLcn.QuadPart);
}

void Mft::loadMftStructure(LCN lcnFirst)
{
	// Pre-calculate and verify some values
	assert(vol->volumeData().BytesPerFileRecordSegment % vol->volumeData().BytesPerSector == 0, "MFT segment size is not a multiple of logical sector size!");
	this->SectorsPerFileSegment = vol->volumeData().BytesPerFileRecordSegment / vol->volumeData().BytesPerSector;
	this->BytesPerFileSegment = vol->volumeData().BytesPerFileRecordSegment;

	auto segment = newSegmentBuf();
	readSegmentLcn(lcnFirst, (FILE_RECORD_SEGMENT_HEADER*)segment.data());
	auto header = (FILE_RECORD_SEGMENT_HEADER*)(segment.data());
	std::cout << "Sector: " << header->SequenceNumber << " " << header->BaseFileRecordSegment.SegmentNumberLowPart << " " << header->Flags << " " << header->FirstAttributeOffset << std::endl;

	//Read attributes
	ATTRIBUTE_RECORD_HEADER* attrData = nullptr;
	for (auto& attr : AttributeIterator(header)) {
		std::cerr << "Attr: " << attr.TypeCode << std::endl;
		if (attr.TypeCode == $DATA) {
			if (!attrData) attrData = &attr;
		}
		if (attr.FormCode == RESIDENT_FORM)
			std::cerr << "  Resident: " << attr.Form.Resident.ValueLength << std::endl;
		else
			for (auto& run : DataRunIterator(&attr))
				std::cerr << "  Run: " << run.offset << ":" << run.length << std::endl;
		assert(attr.TypeCode != $ATTRIBUTE_LIST); //Не поддерживаем $ATTRIBUTE_LIST в MFT!
	}
	assert(attrData != nullptr);
	assert(attrData->FormCode == NONRESIDENT_FORM);
	std::cout << "$Data: " << attrData->Form.Nonresident.LowestVcn << " " << attrData->Form.Nonresident.HighestVcn << std::endl;
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
	//Сегмент может занимать несколько кластеров, тогда читаем, начиная с кластера.
	if (vol->volumeData().ClustersPerFileRecordSegment > 0)
		return this->readSegmentLcn(segmentIndex / vol->volumeData().ClustersPerFileRecordSegment, segment);

	auto BytesPerCluster = vol->volumeData().BytesPerCluster;

	//Иначе сегмент занимает меньше кластера.
	//Нам нужно посчитать его VBN (virtual byte number) и поделить с остатком.
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
	OSCHECKBOOL(SetFilePointerEx(vol->h(), vrbn, nullptr, FILE_BEGIN));
	return readSegmentsNoSeek(segment, count);
}

void Mft::readSegmentsNoSeek(FILE_RECORD_SEGMENT_HEADER* segment, int count)
{
	DWORD bytesRead = 0;
	OSCHECKBOOL(ReadFile(vol->h(), segment, count*BytesPerFileSegment, &bytesRead, nullptr));
	assert(bytesRead == count*BytesPerFileSegment);

	while (count > 0) {
		//So apparently records can be uninitialized. These appear closer to the end of the MFT.
		bool isValidSegment = (*((uint32_t*)(&(segment->MultiSectorHeader.Signature))) == *((uint32_t*)"FILE"));

		//Apply fixups
		if (isValidSegment)
			this->segmentApplyFixups(segment);

		segment = (FILE_RECORD_SEGMENT_HEADER*)((uint8_t*)segment + BytesPerFileSegment);
		count--;
	}
}

void Mft::segmentApplyFixups(FILE_RECORD_SEGMENT_HEADER* header)
{
	auto fixupCnt = header->MultiSectorHeader.UpdateSequenceArraySize - 1; //1 additional cell is for the UpdateValueNumber
	assert(fixupCnt == this->SectorsPerFileSegment);

	auto fixup = (uint16_t*)((char*)header + header->MultiSectorHeader.UpdateSequenceArrayOffset);
	auto pos = (char*)header + vol->volumeData().BytesPerSector - 2;
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



ExclusiveSegmentIterator::ExclusiveSegmentIterator(Mft* mft)
	: mft(mft)
{
	if (mft == nullptr) return; //null-initialized end() iterator
#ifdef SEGMENTITERATOR_BATCHREAD
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
	buffer.resize(batchSize);
#else
	buffer.resize(mft->BytesPerFileSegment);
#endif
	remainingRuns = (int)mft->vcnMap().size();
	if (remainingRuns > 0) {
		currentRun = mft->vcnMap().data();
		remainingRuns--;
		this->openRun();
		this->readCurrent();
	}
}

void ExclusiveSegmentIterator::advanceRun()
{
	std::cout << "advanceRun" << std::endl;
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
void ExclusiveSegmentIterator::openRun()
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
	OSCHECKBOOL(SetFilePointerEx(mft->vol->h(), vrbn, nullptr, FILE_BEGIN));
#ifdef SEGMENTITERATOR_BATCHREAD
	assert(remainingBufferData == 0); //just in case
#endif
}


void ExclusiveSegmentIterator::readCurrent()
{
#ifdef SEGMENTITERATOR_BATCHREAD
	if (remainingBufferData >= mft->BytesPerFileSegment) {
		segment = (FILE_RECORD_SEGMENT_HEADER*)((uint8_t*)segment + mft->BytesPerFileSegment);
		remainingBufferData -= mft->BytesPerFileSegment;
		//Не проверяем, что remainingBufferData делится на BytesPerFileSegment без остатка! Это должно быть верно.
	}
	else {
		//Читаем новый участок, в пределах доступного в этом run
		SegmentNumber segmentCount = (int64_t)(buffer.size()) / mft->BytesPerFileSegment;
		if (segmentCount > remainingSegmentsInRun+1) //1 уже вычтен перед вызовом этого чтения
			segmentCount = remainingSegmentsInRun+1;
		segment = (FILE_RECORD_SEGMENT_HEADER*)(buffer.data());
#ifdef SEGMENTITERATOR_ZEROMEM
		memset(buffer.data(), 0, buffer.size());
#endif
#ifdef SEGMENTITERATOR_EXCLUSIVE
		mft->readSegmentsNoSeek(segment, segmentCount);
#else
		mft->readSegmentsVrbn(vrbn, segment, segmentCount);
#endif
		remainingBufferData = (segmentCount - 1)*(mft->BytesPerFileSegment);
	}
#else
#ifdef SEGMENTITERATOR_EXCLUSIVE
	mft->readSegmentNoSeek((FILE_RECORD_SEGMENT_HEADER*)buffer.data(), 1);
#else
	mft->readSegmentVrbn(vrbn, (FILE_RECORD_SEGMENT_HEADER*)buffer.data(), 1);
#endif
#endif

#ifdef SEGMENTITERATOR_TRACKPOS
	lbn += mft->BytesPerFileSegment;
	vcn++;
#endif
}
