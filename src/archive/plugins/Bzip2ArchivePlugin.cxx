/*
 * Copyright 2003-2019 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/**
  * single bz2 archive handling (requires libbz2)
  */

#include "Bzip2ArchivePlugin.hxx"
#include "../ArchivePlugin.hxx"
#include "../ArchiveFile.hxx"
#include "../ArchiveVisitor.hxx"
#include "input/InputStream.hxx"
#include "input/LocalOpen.hxx"
#include "fs/Path.hxx"

#include <bzlib.h>

#include <stdexcept>

class Bzip2ArchiveFile final : public ArchiveFile {
	std::string name;
	std::shared_ptr<InputStream> istream;

public:
	Bzip2ArchiveFile(Path path, InputStreamPtr &&_is)
		:name(path.GetBase().c_str()),
		 istream(std::move(_is)) {
		// remove .bz2 suffix
		const size_t len = name.length();
		if (len > 4)
			name.erase(len - 4);
	}

	virtual void Visit(ArchiveVisitor &visitor) override {
		visitor.VisitArchiveEntry(name.c_str());
	}

	InputStreamPtr OpenStream(const char *path,
				  Mutex &mutex) override;
};

class Bzip2InputStream final : public InputStream {
	std::shared_ptr<InputStream> input;

	bool eof = false;

	bz_stream bzstream;

	char buffer[5000];

public:
	Bzip2InputStream(const std::shared_ptr<InputStream> &_input,
			 const char *uri,
			 Mutex &mutex);
	~Bzip2InputStream();

	/* virtual methods from InputStream */
	bool IsEOF() const noexcept override;
	size_t Read(std::unique_lock<Mutex> &lock,
		    void *ptr, size_t size) override;

private:
	void Open();
	bool FillBuffer();
};

/* single archive handling allocation helpers */

inline void
Bzip2InputStream::Open()
{
	bzstream.bzalloc = nullptr;
	bzstream.bzfree = nullptr;
	bzstream.opaque = nullptr;

	bzstream.next_in = (char *)buffer;
	bzstream.avail_in = 0;

	int ret = BZ2_bzDecompressInit(&bzstream, 0, 0);
	if (ret != BZ_OK)
		throw std::runtime_error("BZ2_bzDecompressInit() has failed");

	SetReady();
}

/* archive open && listing routine */

static std::unique_ptr<ArchiveFile>
bz2_open(Path pathname)
{
	static Mutex mutex;
	auto is = OpenLocalInputStream(pathname, mutex);
	return std::make_unique<Bzip2ArchiveFile>(pathname, std::move(is));
}

/* single archive handling */

Bzip2InputStream::Bzip2InputStream(const std::shared_ptr<InputStream> &_input,
				   const char *_uri,
				   Mutex &_mutex)
	:InputStream(_uri, _mutex),
	 input(_input)
{
	Open();
}

Bzip2InputStream::~Bzip2InputStream()
{
	BZ2_bzDecompressEnd(&bzstream);
}

InputStreamPtr
Bzip2ArchiveFile::OpenStream(const char *path,
			     Mutex &mutex)
{
	return std::make_unique<Bzip2InputStream>(istream, path, mutex);
}

inline bool
Bzip2InputStream::FillBuffer()
{
	if (bzstream.avail_in > 0)
		return true;

	size_t count = input->LockRead(buffer, sizeof(buffer));
	if (count == 0)
		return false;

	bzstream.next_in = buffer;
	bzstream.avail_in = count;
	return true;
}

size_t
Bzip2InputStream::Read(std::unique_lock<Mutex> &, void *ptr, size_t length)
{
	const ScopeUnlock unlock(mutex);

	int bz_result;
	size_t nbytes = 0;

	if (eof)
		return 0;

	bzstream.next_out = (char *)ptr;
	bzstream.avail_out = length;

	do {
		if (!FillBuffer())
			return 0;

		bz_result = BZ2_bzDecompress(&bzstream);

		if (bz_result == BZ_STREAM_END) {
			eof = true;
			break;
		}

		if (bz_result != BZ_OK)
			throw std::runtime_error("BZ2_bzDecompress() has failed");
	} while (bzstream.avail_out == length);

	nbytes = length - bzstream.avail_out;
	offset += nbytes;

	return nbytes;
}

bool
Bzip2InputStream::IsEOF() const noexcept
{
	return eof;
}

/* exported structures */

static const char *const bz2_extensions[] = {
	"bz2",
	nullptr
};

const ArchivePlugin bz2_archive_plugin = {
	"bz2",
	nullptr,
	nullptr,
	bz2_open,
	bz2_extensions,
};
