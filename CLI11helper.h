#pragma once
#include <string>
#include <map>
#pragma warning(push)
#pragma warning(disable : 4800)
#include <CLI/CLI.hpp>
#pragma warning(pop)


template<typename Enum>
struct EnumItemInfo {
	Enum value;
	std::string name;
	std::string desc;
};

template<typename Enum>
struct EnumNames {};

template<typename Enum, decltype(EnumNames<Enum>::info)* names = nullptr>
inline
std::map<std::string, Enum> enumMap() {
	std::map<std::string, Enum> result{};
	for (auto& info : EnumNames<Enum>::info())
		result.emplace(info.name, info.value);
	return result;
}

template<typename Enum, decltype(EnumNames<Enum>::info)* names = nullptr>
inline
std::string enumName(Enum value) {
	for (auto& pair : EnumNames<Enum>::info())
		if (pair.value == value) return pair.name;
	return std::string{};
}

template<typename Enum, decltype(EnumNames<Enum>::info)* names = nullptr>
inline
std::string enumNames(const std::string& separator) {
	std::string result = {};
	for (auto& pair : EnumNames<Enum>::info())
		result += pair.name + separator;
	result.resize(result.size() - separator.size());
	return result;
}

template<typename Enum, decltype(EnumNames<Enum>::info)* names = nullptr>
inline
std::string enumDescriptions(const std::string& separator) {
	std::string result = {};
	for (auto& pair : EnumNames<Enum>::info())
		result += pair.desc + separator;
	result.resize(result.size() - separator.size());
	return result;
}

template<typename Enum, decltype(EnumNames<Enum>::info)* names = nullptr>
inline
std::string enumNameDesc(const std::string& nameDescSeparator, const std::string& itemSeparator) {
	std::string result = {};
	for (auto& pair : EnumNames<Enum>::info())
		result += pair.name + nameDescSeparator + pair.desc + itemSeparator;
	result.resize(result.size() - itemSeparator.size());
	return result;
}
