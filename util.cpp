#pragma once
#include "util.h"
#include <windows.h>
#include <winioctl.h>
#include <iostream>
#include <vector>
#include <string>
#include "ntfs.h"
#include <codecvt>


Verbosity LogPrinter::verbosity = Verbosity::Info;
NullStream LogPrinter::g_nullStream;
bool LogPrinter::humanReadableSizes = false;


std::string dataSizeToStr(size_t sizeInBytes)
{
	if (!LogPrinter::humanReadableSizes)
		return std::to_string(sizeInBytes);

	static std::string SizeDegrees[7] { "b", "Kb", "Mb", "Gb", "Tb", "Pb" };

	int idx = 0;
	while (sizeInBytes > 0 && idx < 7) {
		if (sizeInBytes < 1024)
			return std::to_string(sizeInBytes) + SizeDegrees[idx];
		sizeInBytes /= 1024;
		idx++;
	}
	return std::to_string(sizeInBytes) + SizeDegrees[idx];
}


std::string wcharToUtf8(const std::wstring& input)
{
	using convert_type = std::codecvt_utf8<wchar_t>;
	static std::wstring_convert<convert_type, wchar_t> converter;
	return converter.to_bytes(input);
}

std::string wcharToUtf8(const wchar_t* first, const wchar_t* last)
{
	using convert_type = std::codecvt_utf8<wchar_t>;
	static std::wstring_convert<convert_type, wchar_t> converter;
	return converter.to_bytes(first, last);
}


std::wstring utf8ToWchar(const std::string& input)
{
	using convert_type = std::codecvt_utf8<wchar_t>;
	static std::wstring_convert<convert_type, wchar_t> converter;
	return converter.from_bytes(input);
}


ProgressCallback::~ProgressCallback()
{
}

void ProgressCallback::setMax(uint64_t value)
{
	this->m_max = value;
}

void ProgressCallback::setOnceEvery(uint64_t value)
{
	this->m_onceEvery = value;
}

void ProgressCallback::progress_int(uint64_t value)
{
}

SimpleConsoleProgressCallback::SimpleConsoleProgressCallback(std::string&& operationName)
	: m_operationName(operationName)
{
}

void SimpleConsoleProgressCallback::progress_int(uint64_t value)
{
	std::cerr << value << " / " << this->m_max << "\r" << std::flush;
}


void FilePrinter::open()
{
	if (outputFile.empty()) return;

	if (outputFile == "-")
		this->out = &std::cout;
	else {
		this->m_fileStream.open(outputFile);
		if (!this->m_fileStream.is_open())
			throwLastOsError();
		this->m_fileStream.seekp(0, std::ofstream::end);
		out = &this->m_fileStream;
	}
}

void FilePrinter::printOne(const std::string& entry)
{
	(*this->out) << entry << std::endl;
}

void FilenamePrinter::printOne(SegmentNumber segmentNo, const std::string& filename, LCN clusterCount)
{
	FilePrinter::printOne(std::to_string(segmentNo) + "\t" + filename + "\t" + std::to_string(clusterCount) + "\t" + dataSizeToStr(clusterCount*BytesPerCluster));
}

