#include "lz4_file_system.hpp"

#include "compression_path_utils.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "lz4.hpp"
#include "lz4_frame_utils.hpp"

#include <cstring>

namespace duckdb {

namespace {

struct Lz4StreamWrapper : public StreamWrapper {
	~Lz4StreamWrapper() override = default;

	void Initialize(QueryContext context, CompressedFile &file, bool write) override; // NOLINT
	bool Read(StreamData &stream_data) override;
	void Write(CompressedFile &file, StreamData &stream_data, data_ptr_t buffer, int64_t nr_bytes) override;
	void Close() override;
	void AbortWrite() override;

private:
	enum class ReadPhase : uint8_t { HEADER, SKIPPABLE, BLOCK, CONTENT_CHECKSUM, NEXT_FRAME, DONE };

	void WriteHeader(CompressedFile &file);
	void FlushPendingBlock(CompressedFile &file);
	void FlushEndMark(CompressedFile &file);
	bool ParseHeader(StreamData &sd);
	bool SkipSkippable(StreamData &sd);
	bool ReadBlock(StreamData &sd);
	void SaveHistory(const_data_ptr_t data, idx_t len);
	static void WriteChild(CompressedFile &file, const void *data, idx_t size);

	CompressedFile *file = nullptr;
	bool writing = false;
	bool block_independent = true;
	bool has_block_checksum = false;
	bool has_content_checksum = false;
	ReadPhase phase = ReadPhase::HEADER;
	uint32_t skippable_remaining = 0;
	XXH32Hasher content_hash;
	unsafe_unique_array<data_t> pending;
	idx_t pending_size = 0;
	unsafe_unique_array<data_t> history;
	idx_t history_size = 0;
};

void Lz4StreamWrapper::Initialize(QueryContext, CompressedFile &file_p, bool write) {
	this->file = &file_p;
	this->writing = write;
	this->phase = ReadPhase::HEADER;
	this->skippable_remaining = 0;
	this->pending_size = 0;
	this->history_size = 0;
	this->block_independent = true;
	this->has_block_checksum = false;
	this->has_content_checksum = write;
	content_hash.Reset();
	if (write) {
		pending = make_unsafe_uniq_array<data_t>(Lz4Frame::WRITE_BLOCK_SIZE);
		WriteHeader(file_p);
	} else {
		history = make_unsafe_uniq_array<data_t>(Lz4Frame::HISTORY_SIZE);
	}
}

void Lz4StreamWrapper::WriteHeader(CompressedFile &file_p) {
	data_t header[7];
	Lz4Frame::StoreLittleEndian32(header, Lz4Frame::MAGIC);
	header[4] = Lz4Frame::WRITE_FLG;
	header[5] = Lz4Frame::WRITE_BD;
	header[6] = data_t((XXH32Hasher::Hash(header + 4, 2) >> 8) & 0xFF);
	WriteChild(file_p, header, sizeof(header));
}

void Lz4StreamWrapper::SaveHistory(const_data_ptr_t data, idx_t len) {
	if (block_independent || !history) {
		return;
	}
	if (len >= Lz4Frame::HISTORY_SIZE) {
		memcpy(history.get(), data + (len - Lz4Frame::HISTORY_SIZE), Lz4Frame::HISTORY_SIZE);
		history_size = Lz4Frame::HISTORY_SIZE;
		return;
	}
	if (history_size + len <= Lz4Frame::HISTORY_SIZE) {
		memcpy(history.get() + history_size, data, len);
		history_size += len;
		return;
	}
	const auto keep = Lz4Frame::HISTORY_SIZE - len;
	memmove(history.get(), history.get() + (history_size - keep), keep);
	memcpy(history.get() + keep, data, len);
	history_size = Lz4Frame::HISTORY_SIZE;
}

bool Lz4StreamWrapper::ParseHeader(StreamData &sd) {
	Lz4InputCursor input(sd);
	if (input.NeedsMore(4, "LZ4 magic number")) {
		return false;
	}
	const auto magic = Lz4Frame::LoadLittleEndian32(input.Current());
	if (Lz4Frame::IsSkippableMagic(magic)) {
		if (input.NeedsMore(8, "LZ4 skippable frame")) {
			return false;
		}
		skippable_remaining = Lz4Frame::LoadLittleEndian32(input.Current() + 4);
		input.Consume(8);
		phase = ReadPhase::SKIPPABLE;
		return SkipSkippable(sd);
	}
	if (magic == Lz4Frame::LEGACY_MAGIC) {
		throw IOException("Legacy LZ4 frames (magic 0x184C2102) are not supported");
	}
	if (magic != Lz4Frame::MAGIC) {
		throw IOException("Input is not an LZ4 frame (unrecognized magic number)");
	}
	if (input.NeedsMore(7, "LZ4 frame descriptor")) {
		return false;
	}
	const auto flg = input.Current()[4];
	const auto bd = input.Current()[5];
	if ((flg & Lz4Frame::FLG_VERSION_MASK) != Lz4Frame::FLG_VERSION) {
		throw IOException("Unsupported LZ4 frame version");
	}
	if (flg & Lz4Frame::FLG_RESERVED) {
		throw IOException("Invalid LZ4 frame: reserved flag is set");
	}
	if (flg & Lz4Frame::FLG_DICTIONARY_ID) {
		throw IOException("LZ4 frames with a dictionary ID are not supported");
	}
	block_independent = (flg & Lz4Frame::FLG_BLOCK_INDEPENDENT) != 0;
	has_block_checksum = (flg & Lz4Frame::FLG_BLOCK_CHECKSUM) != 0;
	has_content_checksum = (flg & Lz4Frame::FLG_CONTENT_CHECKSUM) != 0;
	const bool has_content_size = (flg & Lz4Frame::FLG_CONTENT_SIZE) != 0;

	idx_t descriptor_size = 2;
	if (has_content_size) {
		descriptor_size += 8;
	}
	const auto header_size = 4 + descriptor_size + 1;
	if (input.NeedsMore(header_size, "LZ4 frame header")) {
		return false;
	}
	Lz4Frame::DecodeMaximumBlockSize(bd);
	const auto expected_hc = uint8_t((XXH32Hasher::Hash(input.Current() + 4, descriptor_size) >> 8) & 0xFF);
	const auto actual_hc = input.Current()[4 + descriptor_size];
	if (expected_hc != actual_hc) {
		throw IOException("LZ4 frame header checksum mismatch");
	}
	input.Consume(header_size);
	content_hash.Reset();
	history_size = 0;
	phase = ReadPhase::BLOCK;
	return true;
}

bool Lz4StreamWrapper::SkipSkippable(StreamData &sd) {
	Lz4InputCursor input(sd);
	while (skippable_remaining > 0) {
		auto available = input.Available();
		if (available == 0) {
			if (input.NeedsMore(1, "LZ4 skippable frame payload")) {
				return false;
			}
			available = input.Available();
		}
		const auto skip = MinValue<idx_t>(available, skippable_remaining);
		input.Consume(skip);
		skippable_remaining -= UnsafeNumericCast<uint32_t>(skip);
	}
	phase = ReadPhase::HEADER;
	return true;
}

bool Lz4StreamWrapper::ReadBlock(StreamData &sd) {
	Lz4InputCursor input(sd);
	if (input.NeedsMore(4, "LZ4 block size")) {
		return false;
	}
	const auto raw_size = Lz4Frame::LoadLittleEndian32(input.Current());
	if (raw_size == 0) {
		input.Consume(4);
		phase = has_content_checksum ? ReadPhase::CONTENT_CHECKSUM : ReadPhase::NEXT_FRAME;
		return true;
	}
	const bool uncompressed = (raw_size & Lz4Frame::UNCOMPRESSED_BIT) != 0;
	const auto data_size = idx_t(raw_size & ~Lz4Frame::UNCOMPRESSED_BIT);
	if (data_size > Lz4Frame::MAX_BLOCK_SIZE) {
		throw IOException("LZ4 block exceeds maximum size (%llu bytes)", data_size);
	}
	idx_t needed = 4 + data_size;
	if (has_block_checksum) {
		needed += 4;
	}
	if (input.NeedsMore(needed, "LZ4 block")) {
		return false;
	}
	const auto *block = input.Current() + 4;
	if (has_block_checksum) {
		const auto expected = Lz4Frame::LoadLittleEndian32(block + data_size);
		const auto actual = XXH32Hasher::Hash(block, data_size);
		if (expected != actual) {
			throw IOException("LZ4 block checksum mismatch");
		}
	}

	auto *out = sd.out_buff.get();
	idx_t out_len = 0;
	if (uncompressed) {
		if (data_size > sd.out_buf_size) {
			throw IOException("LZ4 uncompressed block is larger than the output buffer");
		}
		memcpy(out, block, data_size);
		out_len = data_size;
	} else {
		int decoded;
		if (block_independent || history_size == 0) {
			decoded = duckdb_lz4::LZ4_decompress_safe(const_char_ptr_cast(block), char_ptr_cast(out),
			                                          UnsafeNumericCast<int>(data_size),
			                                          UnsafeNumericCast<int>(sd.out_buf_size));
		} else {
			decoded = duckdb_lz4::LZ4_decompress_safe_usingDict(
			    const_char_ptr_cast(block), char_ptr_cast(out), UnsafeNumericCast<int>(data_size),
			    UnsafeNumericCast<int>(sd.out_buf_size), const_char_ptr_cast(history.get()),
			    UnsafeNumericCast<int>(history_size));
		}
		if (decoded < 0) {
			throw IOException("Failed to decompress LZ4 block");
		}
		out_len = UnsafeNumericCast<idx_t>(decoded);
	}
	sd.out_buff_start = out;
	sd.out_buff_end = out + out_len;
	if (has_content_checksum) {
		content_hash.Update(out, out_len);
	}
	SaveHistory(out, out_len);
	input.Consume(needed);
	return true;
}

bool Lz4StreamWrapper::Read(StreamData &sd) {
	D_ASSERT(!writing);
	Lz4InputCursor input(sd);
	while (true) {
		switch (phase) {
		case ReadPhase::HEADER:
			if (!ParseHeader(sd)) {
				return false;
			}
			break;
		case ReadPhase::SKIPPABLE:
			if (!SkipSkippable(sd)) {
				return false;
			}
			break;
		case ReadPhase::BLOCK:
			if (!ReadBlock(sd)) {
				return false;
			}
			if (phase == ReadPhase::BLOCK && sd.out_buff_start != sd.out_buff_end) {
				return false;
			}
			break;
		case ReadPhase::CONTENT_CHECKSUM: {
			if (input.NeedsMore(4, "LZ4 content checksum")) {
				return false;
			}
			const auto expected = Lz4Frame::LoadLittleEndian32(input.Current());
			const auto actual = content_hash.Digest();
			if (expected != actual) {
				throw IOException("LZ4 content checksum mismatch");
			}
			input.Consume(4);
			phase = ReadPhase::NEXT_FRAME;
			break;
		}
		case ReadPhase::NEXT_FRAME:
			if (input.Available() > 0) {
				phase = ReadPhase::HEADER;
				break;
			}
			if (sd.in_buff_end == sd.in_buff.get() + sd.in_buf_size) {
				sd.refresh = true;
				return false;
			}
			phase = ReadPhase::DONE;
			return true;
		case ReadPhase::DONE:
			return true;
		}
	}
}

void Lz4StreamWrapper::FlushPendingBlock(CompressedFile &file_p) {
	if (pending_size == 0) {
		return;
	}
	const auto bound = UnsafeNumericCast<idx_t>(duckdb_lz4::LZ4_compressBound(UnsafeNumericCast<int>(pending_size)));
	auto compressed = make_unsafe_uniq_array<data_t>(bound);
	const auto csize =
	    duckdb_lz4::LZ4_compress_default(const_char_ptr_cast(pending.get()), char_ptr_cast(compressed.get()),
	                                     UnsafeNumericCast<int>(pending_size), UnsafeNumericCast<int>(bound));
	data_t size_buf[4];
	if (csize > 0 && idx_t(csize) < pending_size) {
		Lz4Frame::StoreLittleEndian32(size_buf, UnsafeNumericCast<uint32_t>(csize));
		WriteChild(file_p, size_buf, 4);
		WriteChild(file_p, compressed.get(), UnsafeNumericCast<idx_t>(csize));
	} else {
		Lz4Frame::StoreLittleEndian32(size_buf, UnsafeNumericCast<uint32_t>(pending_size) | Lz4Frame::UNCOMPRESSED_BIT);
		WriteChild(file_p, size_buf, 4);
		WriteChild(file_p, pending.get(), pending_size);
	}
	pending_size = 0;
}

void Lz4StreamWrapper::FlushEndMark(CompressedFile &file_p) {
	data_t end_mark[4] = {0, 0, 0, 0};
	WriteChild(file_p, end_mark, 4);
	if (has_content_checksum) {
		data_t checksum[4];
		Lz4Frame::StoreLittleEndian32(checksum, content_hash.Digest());
		WriteChild(file_p, checksum, 4);
	}
}

void Lz4StreamWrapper::Write(CompressedFile &file_p, StreamData &, data_ptr_t buffer, int64_t nr_bytes) {
	D_ASSERT(writing);
	auto remaining = UnsafeNumericCast<idx_t>(nr_bytes);
	auto *src = buffer;
	while (remaining > 0) {
		const auto space = Lz4Frame::WRITE_BLOCK_SIZE - pending_size;
		const auto copy_size = MinValue<idx_t>(space, remaining);
		memcpy(pending.get() + pending_size, src, copy_size);
		content_hash.Update(src, copy_size);
		pending_size += copy_size;
		src += copy_size;
		remaining -= copy_size;
		if (pending_size == Lz4Frame::WRITE_BLOCK_SIZE) {
			FlushPendingBlock(file_p);
		}
	}
}

void Lz4StreamWrapper::WriteChild(CompressedFile &file, const void *data, idx_t size) {
	file.child_handle->Write(file.context, const_cast<void *>(data), size);
}

void Lz4StreamWrapper::Close() {
	if (!file) {
		return;
	}
	if (writing) {
		FlushPendingBlock(*file);
		FlushEndMark(*file);
	}
	AbortWrite();
}

void Lz4StreamWrapper::AbortWrite() {
	pending.reset();
	history.reset();
	pending_size = 0;
	history_size = 0;
	file = nullptr;
	writing = false;
}

struct Lz4FileSystemHolder {
	Lz4FileSystem lz4_fs;
};

class Lz4File : private Lz4FileSystemHolder, public CompressedFile {
public:
	Lz4File(QueryContext context, unique_ptr<FileHandle> child_handle_p, const string &path, bool write)
	    : CompressedFile(lz4_fs, std::move(child_handle_p), path) {
		Initialize(context, write);
	}

	FileCompressionType GetFileCompressionType() override {
		return FileCompressionType(Lz4FileSystem::COMPRESSION_NAME);
	}
};

} // namespace

unique_ptr<FileHandle> Lz4FileSystem::OpenCompressedFile(QueryContext context, unique_ptr<FileHandle> handle,
                                                         bool write) {
	auto path = handle->path;
	return make_uniq<Lz4File>(context, std::move(handle), path, write);
}

bool Lz4FileSystem::CanHandleFile(const string &fpath) {
	return CompressionPathUtils::HasExtension(fpath, ".lz4");
}

unique_ptr<StreamWrapper> Lz4FileSystem::CreateStream() {
	return make_uniq<Lz4StreamWrapper>();
}

idx_t Lz4FileSystem::InBufferSize() {
	return Lz4Frame::MAX_BLOCK_SIZE + 16;
}

idx_t Lz4FileSystem::OutBufferSize() {
	return Lz4Frame::MAX_BLOCK_SIZE;
}

} // namespace duckdb
