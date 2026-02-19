#pragma once
#include <windows.h>
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
	LCN getLcn(VCN vcn);
	//Некоторые файлы могут быть sparse, но иногда хочется проверить, что дыр нет.
	VCN getFirstMissingVcn();
	void addAttr(ATTRIBUTE_RECORD_HEADER* attr);
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
	Mft(Volume* volume);

	void load();
	void loadMftStructure(LCN lcnFirst);

	std::vector<char> readSegment(LCN lcn);
	//vrOffset = volume-relative byte number
	std::vector<char> readSegmentVrbn(LARGE_INTEGER vrOffset);
	void segmentApplyFixups(FILE_RECORD_SEGMENT_HEADER* header);
};