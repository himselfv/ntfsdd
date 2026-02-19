#pragma once
#include <windows.h>
#include <winioctl.h>
#include <assert.h>
#include <iostream>
#include <vector>
#include <algorithm>
#include "ntfs.h"
#include "ntfsutil.h"
#include "ntfsvolume.h"


/*
Файл описывается одним или несколькими (в разных сегментах) атрибутами $Data,
которые содержат списки подряд идущих DataRun, описывающие VCN с первого по последний указанные для данного атрибута в данном сегменте.

Например:
Сегмент 1:
  $Data VCN0=0, VCN1=100, Runs=0-15,116-200
Сегмент 2:
  $Data VCN0=101, VCN1=200, Runs=301-320,521-600

Мы превращаем всё это в плоскую структуру:
  0: 0
  16: 116
  101: 301
  121: 521
*/
struct VcnMapEntry {
	VCN vcnStart = 0;
	LCN lcnStart = 0;
	LCN len = 0;
	VcnMapEntry(VCN vcnStart, LCN lcnStart, LCN len) : vcnStart(vcnStart), lcnStart(lcnStart), len(len) {}
};
class NonResidentData {
protected:
	std::vector<VcnMapEntry> m_vcnMap;
public:
	LCN getLcn(VCN vcn) {
		for (auto& span : this->m_vcnMap)
			if (vcn < span.vcnStart)
				return (LCN)(-1);
			else if (vcn < span.vcnStart+span.len)
				return (span.lcnStart + vcn - span.vcnStart);
		return (LCN)(-1);
	}
	//Некоторые файлы могут быть sparse, но иногда хочется проверить, что дыр нет.
	VCN getFirstMissingVcn() {
		VCN vcn = 0;
		for (auto& span : this->m_vcnMap)
			if (span.vcnStart > vcn)
				return vcn;
			else
				vcn = span.vcnStart + span.len + 1;
		return (VCN)(-1);
	}
	void addAttr(ATTRIBUTE_RECORD_HEADER* attr) {
		auto vcnpos = 0;
		for (auto& run : DataRunIterator(attr)) {
			m_vcnMap.push_back(VcnMapEntry{ attr->Form.Nonresident.LowestVcn + vcnpos, run.offset, run.length });
			vcnpos += run.length;
		}
		std::sort(m_vcnMap.begin(), m_vcnMap.end(), [](const VcnMapEntry& a, const VcnMapEntry& b) { return a.vcnStart < b.vcnStart; });
	}
};

/*
От MFT нам нужен следующий функционал:
1. Итерация по всем записям.
2. Возможность вытащить запись по её VCN. Зная размер записи, мы можем расчитать LCN + отступ записи в ней.
*/
class Mft : public NonResidentData {
protected:
	Volume* vol;
	int32_t SectorsPerFileSegment = 0;
public:
	Mft(Volume* volume)
	{
		vol = volume;
		// Pre-calculate and verify some values
		assert(vol->volumeData().BytesPerFileRecordSegment % vol->volumeData().BytesPerSector == 0, "MFT segment size is not a multiple of logical sector size!");
		this->SectorsPerFileSegment = vol->volumeData().BytesPerFileRecordSegment / vol->volumeData().BytesPerSector;
	}

	void load()
	{
		loadMftStructure(vol->volumeData().MftStartLcn.QuadPart);
	}

	void loadMftStructure(LCN lcnFirst)
	{
		auto segment = readSegment(lcnFirst);
		auto header = (FILE_RECORD_SEGMENT_HEADER*)(segment.data());

		//Read attributes
		ATTRIBUTE_RECORD_HEADER* attrData = nullptr;
		for (auto& attr : AttributeIterator(header)) {
			std::cerr << "Attr: " << attr.TypeCode << std::endl;
			if (attr.TypeCode == $DATA) {
				if (!attrData) attrData = &attr;
				if (attr.FormCode == RESIDENT_FORM)
					std::cerr << "  Resident: " << attr.Form.Resident.ValueLength;
				else
					for (auto& run : DataRunIterator(&attr))
						std::cerr << "  Run: " << run.offset << ":" << run.length << std::endl;
			}
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
	}


	std::vector<char> readSegment(LCN lcn)
	{
		LARGE_INTEGER vrOffset;
		vrOffset.QuadPart = lcn * vol->volumeData().BytesPerCluster;
		return this->readSegmentVrbn(vrOffset);
	}

	//vrOffset = volume-relative byte number
	std::vector<char> readSegmentVrbn(LARGE_INTEGER vrOffset)
	{
		OSCHECKBOOL(SetFilePointerEx(vol->h(), vrOffset, nullptr, FILE_BEGIN));

		auto segmentSize = vol->volumeData().BytesPerFileRecordSegment;
		std::vector<char> segment;
		segment.resize(segmentSize);

		DWORD bytesRead = 0;
		OSCHECKBOOL(ReadFile(vol->h(), segment.data(), segmentSize, &bytesRead, nullptr));
		assert(bytesRead == segmentSize);

		auto header = (FILE_RECORD_SEGMENT_HEADER*)(segment.data());
		assert(*((uint32_t*)(&(header->MultiSectorHeader.Signature))) == *((uint32_t*)"FILE"));

		std::cout << "Sector: " << header->SequenceNumber << " " << header->BaseFileRecordSegment.SegmentNumberLowPart << " " << header->Flags << " " << header->FirstAttributeOffset << std::endl;

		//Apply fixups
		this->segmentApplyFixups(header);

		return segment;
	}

	void segmentApplyFixups(FILE_RECORD_SEGMENT_HEADER* header)
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
};