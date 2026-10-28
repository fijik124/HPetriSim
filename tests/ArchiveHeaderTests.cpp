#include <afx.h>
#include <catch2/catch_test_macros.hpp>
#include "HpsArchiveHeader.h"

#include <array>
#include <memory>
#include <vector>

namespace
{
struct DeleteArchiveException
{
	void operator()(CArchiveException* error) const { error->Delete(); }
};

void CheckVersionedHeader(const std::vector<BYTE>& bytes)
{
	CMemFile file;
	file.Write(bytes.data(), static_cast<UINT>(bytes.size()));
	file.SeekToBegin();
	CArchive archive(&file, CArchive::load);
	REQUIRE(hps::legacy::ReadDocumentCount(archive) == 0xFFFFFFFF);
	REQUIRE(hps::legacy::ReadDocumentVersion(archive) == 2);
	DWORD followingData = 0;
	archive >> followingData;
	REQUIRE(followingData == 0x12345678);
}

void CheckInvalidVersion(const std::vector<BYTE>& bytes, int expectedCause)
{
	CMemFile file;
	file.Write(bytes.data(), static_cast<UINT>(bytes.size()));
	file.SeekToBegin();
	CArchive archive(&file, CArchive::load);
	try
	{
		hps::legacy::ReadDocumentVersion(archive);
		FAIL("Invalid version data was accepted");
	}
	catch (CArchiveException* error)
	{
		std::unique_ptr<CArchiveException, DeleteArchiveException> ownedError(error);
		REQUIRE(ownedError->m_cause == expectedCause);
	}
}
}

TEST_CASE("Win32 version-2 header leaves the following data intact")
{
	CheckVersionedHeader({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		2, 0, 0x78, 0x56, 0x34, 0x12});
}

TEST_CASE("Historical x64 version-2 header leaves the following data intact")
{
	CheckVersionedHeader({0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
		2, 0, 0x78, 0x56, 0x34, 0x12});
}

TEST_CASE("Writer emits the original eight-byte Win32 header on every architecture")
{
	CMemFile file;
	{
		CArchive archive(&file, CArchive::store);
		hps::legacy::WriteDocumentHeader(archive);
		archive.Close();
	}
	REQUIRE(file.GetLength() == 8);
	file.SeekToBegin();
	std::array<BYTE, 8> actual = {};
	REQUIRE(file.Read(actual.data(), static_cast<UINT>(actual.size())) == actual.size());
	const std::array<BYTE, 8> expected = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 2, 0};
	REQUIRE(actual == expected);
}

TEST_CASE("Unversioned document counts keep their original encoding")
{
	CMemFile file;
	{
		CArchive archive(&file, CArchive::store);
		archive << static_cast<WORD>(17) << static_cast<WORD>(0xFFFF)
			<< static_cast<DWORD>(70000);
		archive.Close();
	}
	file.SeekToBegin();
	CArchive archive(&file, CArchive::load);
	REQUIRE(hps::legacy::ReadDocumentCount(archive) == 17);
	REQUIRE(hps::legacy::ReadDocumentCount(archive) == 70000);
}

TEST_CASE("Malformed x64 marker is rejected")
{
	CheckInvalidVersion({0xFF, 0xFF, 0, 0, 0xFF, 0xFF, 0xFF, 0xFF}, CArchiveException::badSchema);
}

TEST_CASE("Truncated version data is rejected")
{
	CheckInvalidVersion({2}, CArchiveException::endOfFile);
	CheckInvalidVersion({0xFF, 0xFF, 0xFF, 0xFF}, CArchiveException::endOfFile);
}
