///////////////////////////////////////////////////////////////////////////////
//
/// \file       list.c
/// \brief      Listing information about .xz files
//
//  Author:     Lasse Collin
//
//  This file has been put into the public domain.
//  You can do whatever you want with this file.
//
///////////////////////////////////////////////////////////////////////////////

#include "private.h"
#include "tuklib_integer.h"


/// Information about a .xz file
typedef struct {
	/// Combined Index of all Streams in the file
	lzma_index *idx;

	/// Total amount of Stream Padding
	uint64_t stream_padding;

	/// Highest memory usage so far
	uint64_t memusage_max;

	/// True if all Blocks so far have Compressed Size and
	/// Uncompressed Size fields
	bool all_have_sizes;

} xz_file_info;

#define XZ_FILE_INFO_INIT { NULL, 0, 0, true }


/// Information about a .xz Block
typedef struct {
	/// Size of the Block Header
	uint32_t header_size;

	/// A few of the Block Flags as a string
	char flags[3];

	/// Size of the Compressed Data field in the Block
	lzma_vli compressed_size;

	/// Decoder memory usage for this Block
	uint64_t memusage;

	/// The filter chain of this Block in human-readable form
	const char *filter_chain;

} block_header_info;


/// Check ID to string mapping
static const char check_names[LZMA_CHECK_ID_MAX + 1][12] = {
	"None",
	"CRC32",
	"Unknown-2",
	"Unknown-3",
	"CRC64",
	"Unknown-5",
	"Unknown-6",
	"Unknown-7",
	"Unknown-8",
	"Unknown-9",
	"SHA-256",
	"Unknown-11",
	"Unknown-12",
	"Unknown-13",
	"Unknown-14",
	"Unknown-15",
};


/// Value of the Check field as hexadecimal string.
/// This is set by parse_check_value().
static char check_value[2 * LZMA_CHECK_SIZE_MAX + 1];


/// Totals that are displayed if there was more than one file.
/// The "files" counter is also used in print_info_adv() to show
/// the file number.
static struct {
	uint64_t files;
	uint64_t streams;
	uint64_t blocks;
	uint64_t compressed_size;
	uint64_t uncompressed_size;
	uint64_t stream_padding;
	uint64_t memusage_max;
	uint32_t checks;
	bool all_have_sizes;
} totals = { 0, 0, 0, 0, 0, 0, 0, 0, true };


/// \brief      Parse the Index(es) from the given .xz file
///
/// \param      xfi     Pointer to structure where the decoded information
///                     is stored.
/// \param      pair    Input file
///
/// \return     On success, false is returned. On error, true is returned.
///
// TODO: This function is pretty big. liblzma should have a function that
// takes a callback function to parse the Index(es) from a .xz file to make
// it easy for applications.
static bool
parse_indexes(xz_file_info *xfi, file_pair *pair)
{
	if (pair->src_st.st_size <= 0) {
		message_error(_("%s: File is empty"), pair->src_name);
		return true;
	}

	if (pair->src_st.st_size < 2 * LZMA_STREAM_HEADER_SIZE) {
		message_error(_("%s: Too small to be a valid .xz file"),
				pair->src_name);
		return true;
	}

	io_buf buf;
	lzma_stream_flags header_flags;
	lzma_stream_flags footer_flags;
	lzma_ret ret;

	// lzma_stream for the Index decoder
	lzma_stream strm = LZMA_STREAM_INIT;

	// All Indexes decoded so far
	lzma_index *combined_index = NULL;

	// The Index currently being decoded
	lzma_index *this_index = NULL;

	// Current position in the file. We parse the file backwards so
	// initialize it to point to the end of the file.
	off_t pos = pair->src_st.st_size;

	// Each loop iteration decodes one Index.
	do {
		// Check that there is enough data left to contain at least
		// the Stream Header and Stream Footer. This check cannot
		// fail in the first pass of this loop.
		if (pos < 2 * LZMA_STREAM_HEADER_SIZE) {
			message_error("%s: %s", pair->src_name,
					message_strm(LZMA_DATA_ERROR));
			goto error;
		}

		pos -= LZMA_STREAM_HEADER_SIZE;
		lzma_vli stream_padding = 0;

		// Locate the Stream Footer. There may be Stream Padding which
		// we must skip when reading backwards.
		while (true) {
			if (pos < LZMA_STREAM_HEADER_SIZE) {
				message_error("%s: %s", pair->src_name,
						message_strm(
							LZMA_DATA_ERROR));
				goto error;
			}

			if (io_pread(pair, &buf,
					LZMA_STREAM_HEADER_SIZE, pos))
				goto error;

			// Stream Padding is always a multiple of four bytes.
			int i = 2;
			if (buf.u32[i] != 0)
				break;

			// To avoid calling io_pread() for every four bytes
			// of Stream Padding, take advantage that we read
			// 12 bytes (LZMA_STREAM_HEADER_SIZE) already and
			// check them too before calling io_pread() again.
			do {
				stream_padding += 4;
				pos -= 4;
				--i;
			} while (i >= 0 && buf.u32[i] == 0);
		}

		// Decode the Stream Footer.
		ret = lzma_stream_footer_decode(&footer_flags, buf.u8);
		if (ret != LZMA_OK) {
			message_error("%s: %s", pair->src_name,
					message_strm(ret));
			goto error;
		}

		// Check that the size of the Index field looks sane.
		lzma_vli index_size = footer_flags.backward_size;
		if ((lzma_vli)(pos) < index_size + LZMA_STREAM_HEADER_SIZE) {
			message_error("%s: %s", pair->src_name,
					message_strm(LZMA_DATA_ERROR));
			goto error;
		}

		// Set pos to the beginning of the Index.
		pos -= index_size;

		// See how much memory we can use for decoding this Index.
		uint64_t memlimit = hardware_memlimit_get();
		uint64_t memused = 0;
		if (combined_index != NULL) {
			memused = lzma_index_memused(combined_index);
			if (memused > memlimit)
				message_bug();

			memlimit -= memused;
		}

		// Decode the Index.
		ret = lzma_index_decoder(&strm, &this_index, memlimit);
		if (ret != LZMA_OK) {
			message_error("%s: %s", pair->src_name,
					message_strm(ret));
			goto error;
		}

		do {
			// Don't give the decoder more input than the
			// Index size.
			strm.avail_in = my_min(IO_BUFFER_SIZE, index_size);
			if (io_pread(pair, &buf, strm.avail_in, pos))
				goto error;

			pos += strm.avail_in;
			index_size -= strm.avail_in;

			strm.next_in = buf.u8;
			ret = lzma_code(&strm, LZMA_RUN);

		} while (ret == LZMA_OK);

		// If the decoding seems to be successful, check also that
		// the Index decoder consumed as much input as indicated
		// by the Backward Size field.
		if (ret == LZMA_STREAM_END)
			if (index_size != 0 || strm.avail_in != 0)
				ret = LZMA_DATA_ERROR;

		if (ret != LZMA_STREAM_END) {
			// LZMA_BUFFER_ERROR means that the Index decoder
			// would have liked more input than what the Index
			// size should be according to Stream Footer.
			// The message for LZMA_DATA_ERROR makes more
			// sense in that case.
			if (ret == LZMA_BUF_ERROR)
				ret = LZMA_DATA_ERROR;

			message_error("%s: %s", pair->src_name,
					message_strm(ret));

			// If the error was too low memory usage limit,
			// show also how much memory would have been needed.
			if (ret == LZMA_MEMLIMIT_ERROR) {
				uint64_t needed = lzma_memusage(&strm);
				if (UINT64_MAX - needed < memused)
					needed = UINT64_MAX;
				else
					needed += memused;

				message_mem_needed(V_ERROR, needed);
			}

			goto error;
		}

		// Decode the Stream Header and check that its Stream Flags
		// match the Stream Footer.
		pos -= footer_flags.backward_size + LZMA_STREAM_HEADER_SIZE;
		if ((lzma_vli)(pos) < lzma_index_total_size(this_index)) {
			message_error("%s: %s", pair->src_name,
					message_strm(LZMA_DATA_ERROR));
			goto error;
		}

		pos -= lzma_index_total_size(this_index);
		if (io_pread(pair, &buf, LZMA_STREAM_HEADER_SIZE, pos))
			goto error;

		ret = lzma_stream_header_decode(&header_flags, buf.u8);
		if (ret != LZMA_OK) {
			message_error("%s: %s", pair->src_name,
					message_strm(ret));
			goto error;
		}

		ret = lzma_stream_flags_compare(&header_flags, &footer_flags);
		if (ret != LZMA_OK) {
			message_error("%s: %s", pair->src_name,
					message_strm(ret));
			goto error;
		}

		// Store the decoded Stream Flags into this_index. This is
		// needed so that we can print which Check is used in each
		// Stream.
		ret = lzma_index_stream_flags(this_index, &footer_flags);
		if (ret != LZMA_OK)
			message_bug();

		// Store also the size of the Stream Padding field. It is
		// needed to show the offsets of the Streams correctly.
		ret = lzma_index_stream_padding(this_index, stream_padding);
		if (ret != LZMA_OK)
			message_bug();

		if (combined_index != NULL) {
			// Append the earlier decoded Indexes
			// after this_index.
			ret = lzma_index_cat(
					this_index, combined_index, NULL);
			if (ret != LZMA_OK) {
				message_error("%s: %s", pair->src_name,
						message_strm(ret));
				goto error;
			}
		}

		combined_index = this_index;
		this_index = NULL;

		xfi->stream_padding += stream_padding;

	} while (pos > 0);

	lzma_end(&strm);

	// All OK. Make combined_index available to the caller.
	xfi->idx = combined_index;
	return false;

error:
	// Something went wrong, free the allocated memory.
	lzma_end(&strm);
	lzma_index_end(combined_index, NULL);
	lzma_index_end(this_index, NULL);
	return true;
}


/// \brief      Parse the Block Header
///
/// The result is stored into *bhi. The caller takes care of initializing it.
///
/// \return     False on success, true on error.
static bool
parse_block_header(file_pair *pair, const lzma_index_iter *iter,
		block_header_info *bhi, xz_file_info *xfi)
{
#if IO_BUFFER_SIZE < LZMA_BLOCK_HEADER_SIZE_MAX
#	error IO_BUFFER_SIZE < LZMA_BLOCK_HEADER_SIZE_MAX
#endif

	// Get the whole Block Header with one read, but don't read past
	// the end of the Block (or even its Check field).
	const uint32_t size = my_min(iter->block.total_size
				- lzma_check_size(iter->stream.flags->check),
			LZMA_BLOCK_HEADER_SIZE_MAX);
	io_buf buf;
	if (io_pread(pair, &buf, size, iter->block.compressed_file_offset))
		return true;

	// Zero would mean Index Indicator and thus not a valid Block.
	if (buf.u8[0] == 0)
		goto data_error;

	lzma_block block;
	lzma_filter filters[LZMA_FILTERS_MAX + 1];

	// Initialize the pointers so that they can be passed to free().
	for (size_t i = 0; i < ARRAY_SIZE(filters); ++i)
		filters[i].options = NULL;

	// Initialize the block structure and decode Block Header Size.
	block.version = 0;
	block.check = iter->stream.flags->check;
	block.filters = filters;

	block.header_size = lzma_block_header_size_decode(buf.u8[0]);
	if (block.header_size > size)
		goto data_error;

	// Decode the Block Header.
	switch (lzma_block_header_decode(&block, NULL, buf.u8)) {
	case LZMA_OK:
		break;

	case LZMA_OPTIONS_ERROR:
		message_error("%s: %s", pair->src_name,
				message_strm(LZMA_OPTIONS_ERROR));
		return true;

	case LZMA_DATA_ERROR:
		goto data_error;

	default:
		message_bug();
	}

	// Check the Block Flags. These must be done before calling
	// lzma_block_compressed_size(), because it overwrites
	// block.compressed_size.
	bhi->flags[0] = block.compressed_size != LZMA_VLI_UNKNOWN
			? 'c' : '-';
	bhi->flags[1] = block.uncompressed_size != LZMA_VLI_UNKNOWN
			? 'u' : '-';
	bhi->flags[2] = '\0';

	// Collect information if all Blocks have both Compressed Size
	// and Uncompressed Size fields. They can be useful e.g. for
	// multi-threaded decompression so it can be useful to know it.
	xfi->all_have_sizes &= block.compressed_size != LZMA_VLI_UNKNOWN
			&& block.uncompressed_size != LZMA_VLI_UNKNOWN;

	// Validate or set block.compressed_size.
	switch (lzma_block_compressed_size(&block,
			iter->block.unpadded_size)) {
	case LZMA_OK:
		break;

	case LZMA_DATA_ERROR:
		goto data_error;

	default:
		message_bug();
	}

	// Copy the known sizes.
	bhi->header_size = block.header_size;
	bhi->compressed_size = block.compressed_size;

	// Calculate the decoder memory usage and update the maximum
	// memory usage of this Block.
	bhi->memusage = lzma_raw_decoder_memusage(filters);
	if (xfi->memusage_max < bhi->memusage)
		xfi->memusage_max = bhi->memusage;

	// Convert the filter chain to human readable form.
	bhi->filter_chain = message_filters_to_str(filters, false);

	// Free the memory allocated by lzma_block_header_decode().
	for (size_t i = 0; filters[i].id != LZMA_VLI_UNKNOWN; ++i)
		free(filters[i].options);

	return false;

data_error:
	// Show the error message.
	message_error("%s: %s", pair->src_name,
			message_strm(LZMA_DATA_ERROR));

	// Free the memory allocated by lzma_block_header_decode().
	// This is truly needed only if we get here after a succcessful
	// call to lzma_block_header_decode() but it doesn't hurt to
	// always do it.
	for (size_t i = 0; filters[i].id != LZMA_VLI_UNKNOWN; ++i)
		free(filters[i].options);

	return true;
}


/// \brief      Parse the Check field and put it into check_value[]
///
/// \return     False on success, true on error.
static bool
parse_check_value(file_pair *pair, const lzma_index_iter *iter)
{
	// Don't read anything from the file if there is no integrity Check.
	if (iter->stream.flags->check == LZMA_CHECK_NONE) {
		snprintf(check_value, sizeof(check_value), "---");
		return false;
	}

	// Locate and read the Check field.
	const uint32_t size = lzma_check_size(iter->stream.flags->check);
	const off_t offset = iter->block.compressed_file_offset
			+ iter->block.total_size - size;
	io_buf buf;
	if (io_pread(pair, &buf, size, offset))
		return true;

	// CRC32 and CRC64 are in little endian. Guess that all the future
	// 32-bit and 64-bit Check values are little endian too. It shouldn't
	// be a too big problem if this guess is wrong.
	if (size == 4)
		snprintf(check_value, sizeof(check_value),
				"%08" PRIx32, conv32le(buf.u32[0]));
	else if (size == 8)
		snprintf(check_value, sizeof(check_value),
				"%016" PRIx64, conv64le(buf.u64[0]));
	else
		for (size_t i = 0; i < size; ++i)
			snprintf(check_value + i * 2, 3, "%02x", buf.u8[i]);

	return false;
}


/// \brief      Parse detailed information about a Block
///
/// Since this requires seek(s), listing information about all Blocks can
/// be slow.
///
/// \param      pair    Input file
/// \param      iter    Location of the Block whose Check value should
///                     be printed.
/// \param      bhi     Pointer to structure where to store the information
///                     about the Block Header field.
///
/// \return     False on success, true on error. If an error occurs,
///             the error message is printed too so the caller doesn't
///             need to worry about that.
static bool
parse_details(file_pair *pair, const lzma_index_iter *iter,
		block_header_info *bhi, xz_file_info *xfi)
{
	if (parse_block_header(pair, iter, bhi, xfi))
		return true;

	if (parse_check_value(pair, iter))
		return true;

	return false;
}


/// \brief      Get the compression ratio
///
/// This has slightly different format than that is used by in message.c.
static const char *
get_ratio(uint64_t compressed_size, uint64_t uncompressed_size)
{
	if (uncompressed_size == 0)
		return "---";

	const double ratio = (double)(compressed_size)
			/ (double)(uncompressed_size);
	if (ratio > 9.999)
		return "---";

	static char buf[6];
	snprintf(buf, sizeof(buf), "%.3f", ratio);
	return buf;
}


/// \brief      Get a comma-separated list of Check names
///
/// \param      checks  Bit mask of Checks to print
/// \param      space_after_comma
///                     It's better to not use spaces in table-like listings,
///                     but in more verbose formats a space after a comma
///                     is good for readability.
static const char *
get_check_names(uint32_t checks, bool space_after_comma)
{
	assert(checks != 0);

	static char buf[sizeof(check_names)];
	char *pos = buf;
	size_t left = sizeof(buf);

	const char *sep = space_after_comma ? ", " : ",";
	bool comma = false;

	for (size_t i = 0; i <= LZMA_CHECK_ID_MAX; ++i) {
		if (checks & (UINT32_C(1) << i)) {
			my_snprintf(&pos, &left, "%s%s",
					comma ? sep : "", check_names[i]);
			comma = true;
		}
	}

	return buf;
}


static bool
print_info_basic(const xz_file_info *xfi, file_pair *pair)
{
	static bool headings_displayed = false;
	if (!headings_displayed) {
		headings_displayed = true;
		// TRANSLATORS: These are column titles. From Strms (Streams)
		// to Ratio, the columns are right aligned. Check and Filename
		// are left aligned. If you need longer words, it's OK to
		// use two lines here. Test with xz --list.
		puts(_("Strms  Blocks   Compressed Uncompressed  Ratio  "
				"Check   Filename"));
	}

	printf("%5s %7s  %11s  %11s  %5s  %-7s %s\n",
			uint64_to_str(lzma_index_stream_count(xfi->idx), 0),
			uint64_to_str(lzma_index_block_count(xfi->idx), 1),
			uint64_to_nicestr(lzma_index_file_size(xfi->idx),
				NICESTR_B, NICESTR_TIB, false, 2),
			uint64_to_nicestr(
				lzma_index_uncompressed_size(xfi->idx),
				NICESTR_B, NICESTR_TIB, false, 3),
			get_ratio(lzma_index_file_size(xfi->idx),
				lzma_index_uncompressed_size(xfi->idx)),
			get_check_names(lzma_index_checks(xfi->idx), false),
			pair->src_name);

	return false;
}


static void
print_adv_helper(uint64_t stream_count, uint64_t block_count,
		uint64_t compressed_size, uint64_t uncompressed_size,
		uint32_t checks, uint64_t stream_padding)
{
	printf(_("  Streams:            %s\n"),
			uint64_to_str(stream_count, 0));
	printf(_("  Blocks:             %s\n"),
			uint64_to_str(block_count, 0));
	printf(_("  Compressed size:    %s\n"),
			uint64_to_nicestr(compressed_size,
				NICESTR_B, NICESTR_TIB, true, 0));
	printf(_("  Uncompressed size:  %s\n"),
			uint64_to_nicestr(uncompressed_size,
				NICESTR_B, NICESTR_TIB, true, 0));
	printf(_("  Ratio:              %s\n"),
			get_ratio(compressed_size, uncompressed_size));
	printf(_("  Check:              %s\n"),
			get_check_names(checks, true));
	printf(_("  Stream padding:     %s\n"),
			uint64_to_nicestr(stream_padding,
				NICESTR_B, NICESTR_TIB, true, 0));
	return;
}


static bool
print_info_adv(xz_file_info *xfi, file_pair *pair)
{
	// Print the overall information.
	print_adv_helper(lzma_index_stream_count(xfi->idx),
			lzma_index_block_count(xfi->idx),
			lzma_index_file_size(xfi->idx),
			lzma_index_uncompressed_size(xfi->idx),
			lzma_index_checks(xfi->idx),
			xfi->stream_padding);

	// Size of the biggest Check. This is used to calculate the width
	// of the CheckVal field. The table would get insanely wide if
	// we always reserved space for 64-byte Check (128 chars as hex).
	uint32_t check_max = 0;

	// Print information about the Streams.
	puts(_("  Streams:\n    Stream    Blocks"
			"      CompOffset    UncompOffset"
			"        CompSize      UncompSize  Ratio"
			"  Check      Padding"));

	lzma_index_iter iter;
	lzma_index_iter_init(&iter, xfi->idx);

	while (!lzma_index_iter_next(&iter, LZMA_INDEX_ITER_STREAM)) {
		printf("    %6s %9s %15s %15s ",
				uint64_to_str(iter.stream.number, 0),
				uint64_to_str(iter.stream.block_count, 1),
				uint64_to_str(
					iter.stream.compressed_offset, 2),
				uint64_to_str(
					iter.stream.uncompressed_offset, 3));
		printf("%15s %15s  %5s  %-10s %7s\n",
				uint64_to_str(iter.stream.compressed_size, 0),
				uint64_to_str(
					iter.stream.uncompressed_size, 1),
				get_ratio(iter.stream.compressed_size,
					iter.stream.uncompressed_size),
				check_names[iter.stream.flags->check],
				uint64_to_str(iter.stream.padding, 2));

		// Update the maximum Check size.
		if (lzma_check_size(iter.stream.flags->check) > check_max)
			check_max = lzma_check_size(iter.stream.flags->check);
	}

	// Cache the verbosity level to a local variable.
	const bool detailed = message_verbosity_get() >= V_DEBUG;

	// Information collected from Block Headers
	block_header_info bhi;

	// Print information about the Blocks but only if there is
	// at least one Block.
	if (lzma_index_block_count(xfi->idx) > 0) {
		// Calculate the width of the CheckVal field.
		const int checkval_width = my_max(8, 2 * check_max);

		// Print the headings.
		printf(_("  Blocks:\n    Stream     Block"
			"      CompOffset    UncompOffset"
			"       TotalSize      UncompSize  Ratio  Check"));

		if (detailed)
			printf(_("      %-*s  Header  Flags        CompSize"
					"    MemUsage  Filters"),
					checkval_width, _("CheckVal"));

		putchar('\n');

		lzma_index_iter_init(&iter, xfi->idx);

		// Iterate over the Blocks.
		while (!lzma_index_iter_next(&iter, LZMA_INDEX_ITER_BLOCK)) {
			if (detailed && parse_details(pair, &iter, &bhi, xfi))
					return true;

			printf("    %6s %9s %15s %15s ",
				uint64_to_str(iter.stream.number, 0),
				uint64_to_str(
					iter.block.number_in_stream, 1),
				uint64_to_str(
					iter.block.compressed_file_offset, 2),
				uint64_to_str(
					iter.block.uncompressed_file_offset,
					3));
			printf("%15s %15s  %5s  %-*s",
				uint64_to_str(iter.block.total_size, 0),
				uint64_to_str(iter.block.uncompressed_size,
						1),
				get_ratio(iter.block.total_size,
					iter.block.uncompressed_size),
				detailed ? 11 : 1,
				check_names[iter.stream.flags->check]);

			if (detailed) {
				// Show MiB for memory usage, because it
				// is the only size which is not in bytes.
				const lzma_vli compressed_size
						= iter.block.unpadded_size
						- bhi.header_size
						- lzma_check_size(
						iter.stream.flags->check);
				printf("%-*s  %6s  %-5s %15s %7s MiB  %s",
					checkval_width, check_value,
					uint64_to_str(bhi.header_size, 0),
					bhi.flags,
					uint64_to_str(compressed_size, 1),
					uint64_to_str(
						round_up_to_mib(bhi.memusage),
						2),
					bhi.filter_chain);
			}

			putchar('\n');
		}
	}

	if (detailed) {
		printf(_("  Memory needed:      %s MiB\n"), uint64_to_str(
				round_up_to_mib(xfi->memusage_max), 0));
		printf(_("  Sizes in headers:   %s\n"),
				xfi->all_have_sizes ? _("Yes") : _("No"));
	}

	return false;
}


static bool
print_info_robot(xz_file_info *xfi, file_pair *pair)
{
	printf("name\t%s\n", pair->src_name);

	printf("file\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
			"\t%s\t%s\t%" PRIu64 "\n",
			lzma_index_stream_count(xfi->idx),
			lzma_index_block_count(xfi->idx),
			lzma_index_file_size(xfi->idx),
			lzma_index_uncompressed_size(xfi->idx),
			get_ratio(lzma_index_file_size(xfi->idx),
				lzma_index_uncompressed_size(xfi->idx)),
			get_check_names(lzma_index_checks(xfi->idx), false),
			xfi->stream_padding);

	if (message_verbosity_get() >= V_VERBOSE) {
		lzma_index_iter iter;
		lzma_index_iter_init(&iter, xfi->idx);

		while (!lzma_index_iter_next(&iter, LZMA_INDEX_ITER_STREAM))
			printf("stream\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
				"\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
				"\t%s\t%s\t%" PRIu64 "\n",
				iter.stream.number,
				iter.stream.block_count,
				iter.stream.compressed_offset,
				iter.stream.uncompressed_offset,
				iter.stream.compressed_size,
				iter.stream.uncompressed_size,
				get_ratio(iter.stream.compressed_size,
					iter.stream.uncompressed_size),
				check_names[iter.stream.flags->check],
				iter.stream.padding);

		lzma_index_iter_rewind(&iter);
		block_header_info bhi;

		while (!lzma_index_iter_next(&iter, LZMA_INDEX_ITER_BLOCK)) {
			if (message_verbosity_get() >= V_DEBUG
					&& parse_details(
						pair, &iter, &bhi, xfi))
				return true;

			printf("block\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
					"\t%" PRIu64 "\t%" PRIu64
					"\t%" PRIu64 "\t%" PRIu64 "\t%s\t%s",
					iter.stream.number,
					iter.block.number_in_stream,
					iter.block.number_in_file,
					iter.block.compressed_file_offset,
					iter.block.uncompressed_file_offset,
					iter.block.total_size,
					iter.block.uncompressed_size,
					get_ratio(iter.block.total_size,
						iter.block.uncompressed_size),
					check_names[iter.stream.flags->check]);

			if (message_verbosity_get() >= V_DEBUG)
				printf("\t%s\t%" PRIu32 "\t%s\t%" PRIu64
						"\t%" PRIu64 "\t%s",
						check_value,
						bhi.header_size,
						bhi.flags,
						bhi.compressed_size,
						bhi.memusage,
						bhi.filter_chain);

			putchar('\n');
		}
	}

	if (message_verbosity_get() >= V_DEBUG)
		printf("summary\t%" PRIu64 "\t%s\n",
				xfi->memusage_max,
				xfi->all_have_sizes ? "yes" : "no");

	return false;
}


static void
update_totals(const xz_file_info *xfi)
{
	// TODO: Integer overflow checks
	++totals.files;
	totals.streams += lzma_index_stream_count(xfi->idx);
	totals.blocks += lzma_index_block_count(xfi->idx);
	totals.compressed_size += lzma_index_file_size(xfi->idx);
	totals.uncompressed_size += lzma_index_uncompressed_size(xfi->idx);
	totals.stream_padding += xfi->stream_padding;
	totals.checks |= lzma_index_checks(xfi->idx);

	if (totals.memusage_max < xfi->memusage_max)
		totals.memusage_max = xfi->memusage_max;

	totals.all_have_sizes &= xfi->all_have_sizes;

	return;
}


static void
print_totals_basic(void)
{
	// Print a separator line.
	char line[80];
	memset(line, '-', sizeof(line));
	line[sizeof(line) - 1] = '\0';
	puts(line);

	// Print the totals except the file count, which needs
	// special handling.
	printf("%5s %7s  %11s  %11s  %5s  %-7s ",
			uint64_to_str(totals.streams, 0),
			uint64_to_str(totals.blocks, 1),
			uint64_to_nicestr(totals.compressed_size,
				NICESTR_B, NICESTR_TIB, false, 2),
			uint64_to_nicestr(totals.uncompressed_size,
				NICESTR_B, NICESTR_TIB, false, 3),
			get_ratio(totals.compressed_size,
				totals.uncompressed_size),
			get_check_names(totals.checks, false));

	// Since we print totals only when there are at least two files,
	// the English message will always use "%s files". But some other
	// languages need different forms for different plurals so we
	// have to translate this string still.
	//
	// TRANSLATORS: This simply indicates the number of files shown
	// by --list even though the format string uses %s.
	printf(N_("%s file", "%s files\n",
			totals.files <= ULONG_MAX ? totals.files
				: (totals.files % 1000000) + 1000000),
			uint64_to_str(totals.files, 0));

	return;
}


static void
print_totals_adv(void)
{
	putchar('\n');
	puts(_("Totals:"));
	printf(_("  Number of files:    %s\n"),
			uint64_to_str(totals.files, 0));
	print_adv_helper(totals.streams, totals.blocks,
			totals.compressed_size, totals.uncompressed_size,
			totals.checks, totals.stream_padding);

	if (message_verbosity_get() >= V_DEBUG) {
		printf(_("  Memory needed:      %s MiB\n"), uint64_to_str(
				round_up_to_mib(totals.memusage_max), 0));
		printf(_("  Sizes in headers:   %s\n"),
				totals.all_have_sizes ? _("Yes") : _("No"));
	}

	return;
}


static void
print_totals_robot(void)
{
	printf("totals\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64 "\t%" PRIu64
			"\t%s\t%s\t%" PRIu64 "\t%" PRIu64,
			totals.streams,
			totals.blocks,
			totals.compressed_size,
			totals.uncompressed_size,
			get_ratio(totals.compressed_size,
				totals.uncompressed_size),
			get_check_names(totals.checks, false),
			totals.stream_padding,
			totals.files);

	if (message_verbosity_get() >= V_DEBUG)
		printf("\t%" PRIu64 "\t%s",
				totals.memusage_max,
				totals.all_have_sizes ? "yes" : "no");

	putchar('\n');

	return;
}


extern void
list_totals(void)
{
	if (opt_robot) {
		// Always print totals in --robot mode. It can be convenient
		// in some cases and doesn't complicate usage of the
		// single-file case much.
		print_totals_robot();

	} else if (totals.files > 1) {
		// For non-robot mode, totals are printed only if there
		// is more than one file.
		if (message_verbosity_get() <= V_WARNING)
			print_totals_basic();
		else
			print_totals_adv();
	}

	return;
}


extern void
list_file(const char *filename)
{
	if (opt_format != FORMAT_XZ && opt_format != FORMAT_AUTO)
		message_fatal(_("--list works only on .xz files "
				"(--format=xz or --format=auto)"));

	message_filename(filename);

	if (filename == stdin_filename) {
		message_error(_("--list does not support reading from "
				"standard input"));
		return;
	}

	// Unset opt_stdout so that io_open_src() won't accept special files.
	// Set opt_force so that io_open_src() will follow symlinks.
	opt_stdout = false;
	opt_force = true;
	file_pair *pair = io_open_src(filename);
	if (pair == NULL)
		return;

	xz_file_info xfi = XZ_FILE_INFO_INIT;
	if (!parse_indexes(&xfi, pair)) {
		bool fail;

		// We have three main modes:
		//  - --robot, which has submodes if --verbose is specified
		//    once or twice
		//  - Normal --list without --verbose
		//  - --list with one or two --verbose
		if (opt_robot)
			fail = print_info_robot(&xfi, pair);
		else if (message_verbosity_get() <= V_WARNING)
			fail = print_info_basic(&xfi, pair);
		else
			fail = print_info_adv(&xfi, pair);

		// Update the totals that are displayed after all
		// the individual files have been listed. Don't count
		// broken files.
		if (!fail)
			update_totals(&xfi);

		lzma_index_end(xfi.idx, NULL);
	}

	io_close(pair, false);
	return;
}
