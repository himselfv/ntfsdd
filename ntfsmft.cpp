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


Mft::Mft(Volume* volume)
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

	return this->readSegmentVrbn(vrbn, segment);
}

//Читает ПЕРВЫЙ сегмент в указанном логическом кластере. Их там может быть несколько!
void Mft::readSegmentLcn(LCN lcn, FILE_RECORD_SEGMENT_HEADER* segment)
{
	VRBN vrOffset;
	vrOffset.QuadPart = lcn * vol->volumeData().BytesPerCluster;
	return this->readSegmentVrbn(vrOffset, segment);
}

void Mft::readSegmentVrbn(VRBN vrbn, FILE_RECORD_SEGMENT_HEADER* segment)
{
	OSCHECKBOOL(SetFilePointerEx(vol->h(), vrbn, nullptr, FILE_BEGIN));
	return readSegmentNoSeek(segment);
}

void Mft::readSegmentNoSeek(FILE_RECORD_SEGMENT_HEADER* segment)
{
	DWORD bytesRead = 0;
	OSCHECKBOOL(ReadFile(vol->h(), segment, BytesPerFileSegment, &bytesRead, nullptr));
	assert(bytesRead == BytesPerFileSegment);

	//So apparently records can be uninitialized. These appear closer to the end of the MFT.
	bool isValidSegment = (*((uint32_t*)(&(segment->MultiSectorHeader.Signature))) == *((uint32_t*)"FILE"));

	//Apply fixups
	if (isValidSegment)
		this->segmentApplyFixups(segment);
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

ExclusiveSegmentIterator::ExclusiveSegmentIterator(Mft* mft)
	: mft(mft)
{
	if (mft == nullptr) return; //null-initialized end() iterator
	segment.resize(mft->BytesPerFileSegment);
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
}
