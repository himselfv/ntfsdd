#pragma once
#include <windows.h>
#include <winioctl.h>
#include <iostream>
#include <vector>
#include <string>
#include "ntfs.h"
#include "util.h"

/*
LCN runs in nonresident attributes are variable-sized, from 1 to 15 bytes in length (in practice 8 bytes should be more than enough).
They have to be sign-extended: the target variable (8 bytes) has to inherit sign of the lower-byte (1-3-5 byte) source.
*/
int64_t ReadSignedValue(const uint8_t* buffer, size_t size) {
	int64_t value = 0;
	// Copy the bytes into our 64-bit integer
	for (size_t i = 0; i < size; ++i) {
		value |= (int64_t)buffer[i] << (i * 8);
	}

	// Sign-extend: Check if the last byte's high bit is set
	if (size > 0 && (buffer[size - 1] & 0x80)) {
		// Fill the remaining leading bits with 1s
		for (size_t i = size; i < sizeof(int64_t); ++i) {
			value |= (int64_t)0xFF << (i * 8);
		}
	}
	return value;
}

int64_t ReadUnsignedValue(const uint8_t* buffer, size_t size) {
	int64_t value = 0;
	// Copy the bytes into our 64-bit integer
	for (size_t i = 0; i < size; ++i) {
		value |= (int64_t)buffer[i] << (i * 8);
	}
	return value;
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
		this->m_fileStream.seekp(std::ofstream::end, 0);
		out = &this->m_fileStream;
	}
}

void FilePrinter::printOne(const std::string& entry)
{
	(*this->out) << entry << std::endl;
}

