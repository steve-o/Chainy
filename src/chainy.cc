/* UPA interactive fake snapshot provider.
 */

#include "chainy.hh"

#define __STDC_FORMAT_MACROS
#include <cstdint>
#include <inttypes.h>

#include <windows.h>

/* UPA 8.0 */   
#include <upa/upa.h>
#include <rtr/rsslPayloadCache.h>
#include <rtr/rsslPayloadEntry.h>

#include "chromium/command_line.hh"
#include "chromium/files/file_util.hh"
#include "chromium/logging.hh"
#include "chromium/strings/string_split.hh"
#include "upa.hh"
#include "upaostream.hh"
#include "unix_epoch.hh"


namespace switches {

//   Symbol map file.
const char kSymbolPath[]		= "symbol-path";

}  // namespace switches

namespace {

static const std::string kErrorMalformedRequest = "Malformed request.";
static const std::string kErrorNotFound = "Not found in symbol set.";
static const std::string kErrorPermData = "Unable to retrieve permission data for item.";
static const std::string kErrorInternal = "Internal error.";

}  // namespace anon

static std::weak_ptr<chainy::chainy_t> g_application;

chainy::chainy_t::chainy_t()
	: consumer_shutdown_ (false)
	, provider_shutdown_ (false)
	, shutting_down_ (false)
{
}

chainy::chainy_t::~chainy_t()
{
	LOG(INFO) << "fin.";
}

/* On a shutdown event set a global flag and force the event queue
 * to catch the event by submitting a log event.
 */
static
BOOL
CtrlHandler (
	DWORD	fdwCtrlType
	)
{
	const char* message;
	switch (fdwCtrlType) {
	case CTRL_C_EVENT:
		message = "Caught ctrl-c event";
		break;
	case CTRL_CLOSE_EVENT:
		message = "Caught close event";
		break;
	case CTRL_BREAK_EVENT:
		message = "Caught ctrl-break event";
		break;
	case CTRL_LOGOFF_EVENT:
		message = "Caught logoff event";
		break;
	case CTRL_SHUTDOWN_EVENT:
	default:
		message = "Caught shutdown event";
		break;
	}
	if (!g_application.expired()) {
		LOG(INFO) << message << "; closing app.";
		auto sp = g_application.lock();
		sp->Quit();
	} else {
		LOG(WARNING) << message << "; provider already expired.";
	}
	return TRUE;
}

int
chainy::chainy_t::Run()
{
	int rc = EXIT_SUCCESS;
	VLOG(1) << "Run as application starting.";
/* Add shutdown handler. */
	g_application = shared_from_this();
	::SetConsoleCtrlHandler ((PHANDLER_ROUTINE)::CtrlHandler, TRUE);
	if (Start()) {
/* Wait for mainloop to quit */
		boost::unique_lock<boost::mutex> provider_lock (provider_lock_);
		boost::unique_lock<boost::mutex> consumer_lock (consumer_lock_);
		while (!provider_shutdown_)
			provider_cond_.wait (provider_lock);
		while (!consumer_shutdown_)
			consumer_cond_.wait (consumer_lock);
		Reset();
	} else {
		rc = EXIT_FAILURE;
	}
/* Remove shutdown handler. */
	::SetConsoleCtrlHandler ((PHANDLER_ROUTINE)::CtrlHandler, FALSE);
	VLOG(1) << "Run as application finished.";
	return rc;
}

void
chainy::chainy_t::Quit()
{
	shutting_down_ = true;
	if ((bool)consumer_) {
		LOG(INFO) << "Closing consumer.";
		consumer_->Quit();
	}
	if ((bool)provider_) {
		LOG(INFO) << "Closing provider.";
		provider_->Quit();
	}
}

bool
chainy::chainy_t::Initialize ()
{
	LOG(INFO) << "Chainy McChainface: { "
		"\"config\": " << config_ <<
		" }";

	std::vector<std::string> instruments;

	try {
/* Configuration. */
		CommandLine* command_line = CommandLine::ForCurrentProcess();

/* Symbol list */
		if (command_line->HasSwitch (switches::kSymbolPath)) {
			config_.symbol_path = command_line->GetSwitchValueASCII (switches::kSymbolPath);
			if (chromium::PathExists (config_.symbol_path)) {
				std::string contents;
				file_util::ReadFileToString (config_.symbol_path, &contents);
				chromium::SplitString (contents, '\n', &instruments);
			}
			LOG(INFO) << "Symbol set contains " << instruments.size() << " entries.";
		} else {
			LOG(WARNING) << "No symbol file provided.";
		}

/* UPA context. */
		upa_.reset (new upa_t (config_));
		if (!(bool)upa_ || !upa_->Initialize())
			goto cleanup;

/* UPA provider. */
		provider_.reset (new provider_t (config_, upa_, static_cast<client_t::Delegate*> (this)));
		if (!(bool)provider_)
			goto cleanup;

/* UPA consumer. */
		consumer_.reset (new consumer_t (config_, upa_, static_cast<consumer_t::Delegate*> (this)));
		if (!(bool)consumer_ || !consumer_->Initialize())
			goto cleanup;

		if (!provider_->Initialize (consumer_.get(), consumer_.get()))
			goto cleanup;

/* Create state for subscribed RIC. */
		for (const auto& instrument : instruments) {
			if (instrument.empty())
				continue;
			auto stream = std::make_shared<subscription_stream_t> ();
			if (!(bool)stream)
				return false;
			stream->links.push_back (stream);
			if (consumer_->CreateItemStream (instrument.c_str(), stream))
				streams_.insert (std::make_pair (instrument, stream));
			else
				LOG(WARNING) << "Cannot create stream for \"" << instrument << "\".";
			VLOG(1) << instrument;
		}

	} catch (const std::exception& e) {
		LOG(ERROR) << "Upa::Initialisation exception: { "
			"\"What\": \"" << e.what() << "\" }";
	}

	LOG(INFO) << "Initialisation complete.";
	return true;
cleanup:
	Reset();
	LOG(INFO) << "Initialisation failed.";
	return false;
}

bool
chainy::chainy_t::OnSync()
{
	DVLOG(3) << "Sync";

	for (auto it : streams_)
	{
		auto stream = it.second.get();
		if (nullptr == stream->payload_entry_handle) {
			LOG(WARNING) << "Payload entry handle for \"" << it.first << "\" is null.";
			continue;
		}
		DVLOG(3) << "Sync for \"" << it.first << "\"";
	}

/* enable provider only with synchronised consumer. */
	provider_->SetAcceptingRequests (true);
	DVLOG(3) << "/Sync";
	return true;
}

bool
chainy::chainy_t::OnTrigger()
{
	LOG(INFO) << "Trigger";

	for (auto it : streams_)
	{
		auto stream = it.second.get();
		if (nullptr == stream->payload_entry_handle) {
			LOG(WARNING) << "Payload entry handle for \"" << it.first << "\" is null.";
			continue;
		}
		if (0 != stream->snapshot_handle.load()) {
			LOG(INFO) << "Snapshot handle for \"" << it.first << "\" is non-null.";
			continue;
		}
		LOG(INFO) << "Trigger for \"" << it.first << "\"";
/* copy-on-write snapshot */
		stream->snapshot_handle.store ((uintptr_t)(void*)stream->payload_entry_handle);
	}

	LOG(INFO) << "/Trigger";
	return true;
}

/* Check snapshot trigger:
 * Elektron stream is ordered per item stream not cross item stream.  Thus it is possible
 * to receive MSFT@11:01 before AAPL@11:00.  One can perform a snapshot if an update is
 * received post the target timestamp, but one has to wait for a delay time x after the
 * target timestamp to capture low-liquidity instruments.
 *
 * Return true to perform snapshot before applying update, return false to only apply the update.
 */

bool
chainy::chainy_t::CheckTrigger (
	const uint8_t rwf_major_version,
	const uint8_t rwf_minor_version, 
	RsslMsg* msg
	)
{
	RsslDecodeIterator it = RSSL_INIT_DECODE_ITERATOR;
	RsslFieldList field_list = RSSL_INIT_FIELD_LIST;
	RsslFieldEntry field_entry = RSSL_INIT_FIELD_ENTRY;
	RsslBuffer rssl_buffer;
	RsslRet rc;

	rc = rsslSetDecodeIteratorRWFVersion (&it, rwf_major_version, rwf_minor_version);
	if (RSSL_RET_SUCCESS != rc) {
		LOG(ERROR) << "rsslSetDecodeIteratorRWFVersion: { "
                          "\"returnCode\": " << static_cast<signed> (rc) << ""
                        ", \"enumeration\": \"" << rsslRetCodeToString (rc) << "\""
                        ", \"text\": \"" << rsslRetCodeInfo (rc) << "\""
                        ", \"majorVersion\": " << static_cast<unsigned> (rwf_major_version) << ""
                        ", \"minorVersion\": " << static_cast<unsigned> (rwf_minor_version) << ""
                        " }";
                return false;
        }
	rc = rsslSetDecodeIteratorBuffer (&it, &msg->msgBase.encDataBody);
	if (RSSL_RET_SUCCESS != rc) {
		LOG(ERROR) << "rsslSetDecodeIteratorBuffer: { "
                          "\"returnCode\": " << static_cast<signed> (rc) << ""
                        ", \"enumeration\": \"" << rsslRetCodeToString (rc) << "\""
                        ", \"text\": \"" << rsslRetCodeInfo (rc) << "\""
                        " }";
                return false;
        }
	rc = rsslDecodeFieldList (&it, &field_list, nullptr);
	if (RSSL_RET_SUCCESS != rc) {
		LOG(ERROR) << "rsslDecodeFieldList: { "
                          "\"returnCode\": " << static_cast<signed> (rc) << ""
                        ", \"enumeration\": \"" << rsslRetCodeToString (rc) << "\""
                        ", \"text\": \"" << rsslRetCodeInfo (rc) << "\""
			" }";
		return false;
	}
static const unsigned target_hour = 11 + 5 - 1;
	while (RSSL_RET_SUCCESS == (rc = rsslDecodeFieldEntry (&it, &field_entry))) {
		switch (field_entry.fieldId) {
/* Unusual decode errors include:
 *
 * rsslDecodeTime: { "returnCode": -26, "enumeration": "RSSL_RET_INCOMPLETE_DATA",
 *                   "text": "Failure: Not enough data was provided." }
 * rsslDecodeTime: { "returnCode": 15, "enumeration": "RSSL_RET_BLANK_DATA",
 *                   "text": "Success: Decoded data is a Blank." }
 *
 * Blank timestamp should appear pre-market open on exchange reset.
 */
		case 238:
		case 815:
			rc = rsslDecodeBuffer (&it, &rssl_buffer);
			if (RSSL_RET_BLANK_DATA == rc) {
				LOG(INFO) << field_entry.fieldId << " = <blank>";
				return false;
			}
			if (RSSL_RET_SUCCESS != rc) {
				LOG(ERROR) << "rsslDecodeBuffer: { "
		                          "\"returnCode\": " << static_cast<signed> (rc) << ""
		                        ", \"enumeration\": \"" << rsslRetCodeToString (rc) << "\""
		                        ", \"text\": \"" << rsslRetCodeInfo (rc) << "\""
					" }";
				return false;
			}
			LOG(INFO) << field_entry.fieldId << " = \"" << std::string (rssl_buffer.data, rssl_buffer.length) << "\"";
			return true;
		default:
			break;
		}
	}
	return false;
}

/* The payload cache has updated, update symbol-list image for publishing.
 *
 * Returns false to abort update processing.
 */

bool
chainy::chainy_t::OnWrite (
	std::shared_ptr<item_stream_t> item_stream,
	const uint8_t rwf_major_version,
	const uint8_t rwf_minor_version,
	RsslMsg* msg
	)
{
	DVLOG(3) << "OnWrite";
	auto stream = std::static_pointer_cast<subscription_stream_t> (item_stream);
	std::vector<std::string> v;
	bool is_complete = false;
	RsslDecodeIterator it;
	RsslFieldList field_list;
	RsslFieldEntry field_entry;
	RsslBuffer rssl_buffer;
	RsslCacheError rssl_cache_err;
	RsslRet rc;

	std::shared_ptr<subscription_stream_t> parent = stream->links.front();

	rsslClearDecodeIterator (&it);
	rsslCacheErrorClear (&rssl_cache_err);

	rc = rsslSetDecodeIteratorRWFVersion (&it, rwf_major_version, rwf_minor_version);
	if (RSSL_RET_SUCCESS != rc) {
		LOG(ERROR) << "rsslSetDecodeIteratorRWFVersion: { "
			  "\"returnCode\": " << static_cast<signed> (rc) << ""
			", \"enumeration\": \"" << rsslRetCodeToString (rc) << "\""
			", \"text\": \"" << rsslRetCodeInfo (rc) << "\""
			", \"majorVersion\": " << static_cast<unsigned> (rwf_major_version) << ""
			", \"minorVersion\": " << static_cast<unsigned> (rwf_minor_version) << ""
			" }";
		return false;
	}
	rc = rsslSetDecodeIteratorBuffer (&it, &msg->msgBase.encDataBody);
	if (RSSL_RET_SUCCESS != rc) {
		LOG(ERROR) << "rsslSetDecodeIteratorBuffer: { "
			  "\"returnCode\": " << static_cast<signed> (rc) << ""
			", \"enumeration\": \"" << rsslRetCodeToString (rc) << "\""
			", \"text\": \"" << rsslRetCodeInfo (rc) << "\""
			" }";
		return false;
	}
	rc = rsslDecodeFieldList (&it, &field_list, nullptr);
	if (RSSL_RET_SUCCESS != rc) {
		LOG(ERROR) << "rsslDecodeFieldList: { "
			  "\"returnCode\": " << static_cast<signed> (rc) << ""
			", \"enumeration\": \"" << rsslRetCodeToString (rc) << "\""
			", \"text\": \"" << rsslRetCodeInfo (rc) << "\""
			" }";
		return false;
	}
	while (RSSL_RET_SUCCESS == (rc = rsslDecodeFieldEntry (&it, &field_entry))) {
		switch (field_entry.fieldId) {
		case 240:
		case 241:
		case 242:
		case 243:
		case 244:
		case 245:
		case 246:
		case 247:
		case 248:
		case 249:
		case 250:
		case 251:
		case 252:
		case 253:
/* long versions */
		case 800:
		case 801:
		case 802:
		case 803:
		case 804:
		case 805:
		case 806:
		case 807:
		case 808:
		case 809:
		case 810:
		case 811:
		case 812:
		case 813:
			rc = rsslDecodeBuffer (&it, &rssl_buffer);
			if (RSSL_RET_BLANK_DATA == rc || 0 == rssl_buffer.length) {
				VLOG(1) << field_entry.fieldId << " = <blank>";
				continue;
			}
			if (RSSL_RET_SUCCESS != rc) {
				LOG(ERROR) << "rsslDecodeBuffer: { "
					  "\"returnCode\": " << static_cast<signed> (rc) << ""
					", \"enumeration\": \"" << rsslRetCodeToString (rc) << "\""
					", \"text\": \"" << rsslRetCodeInfo (rc) << "\""
					" }";
				return false;
			}
			VLOG(1) << field_entry.fieldId << " = \"" << std::string (rssl_buffer.data, rssl_buffer.length) << "\"";
			v.emplace_back (rssl_buffer.data, rssl_buffer.length);
			break;

/* next link pointers */
		case 238:
		case 815:
			rc = rsslDecodeBuffer (&it, &rssl_buffer);
			if (RSSL_RET_BLANK_DATA == rc || 0 == rssl_buffer.length) {
				VLOG(1) << "<next link> = <blank>";
				continue;
			}
			if (RSSL_RET_SUCCESS != rc) {
				LOG(ERROR) << "rsslDecodeBuffer: { "
					  "\"returnCode\": " << static_cast<signed> (rc) << ""
					", \"enumeration\": \"" << rsslRetCodeToString (rc) << "\""
					", \"text\": \"" << rsslRetCodeInfo (rc) << "\""
					" }";
				return false;
			}
			VLOG(1) << "<next link> = \"" << std::string (rssl_buffer.data, rssl_buffer.length) << "\"";
			if (0 == rssl_buffer.length) {
				is_complete = true;
/* destroy all following links */
				parent->links.resize (1 + stream->index);
			} else {
				std::string link_name (rssl_buffer.data, rssl_buffer.length);
				auto link_stream = std::make_shared<subscription_stream_t> ();
				if (!(bool)link_stream)
					return false;
				link_stream->links.push_back (parent);
				link_stream->index = 1 + stream->index;
				if (consumer_->CreateItemStream (link_name.c_str(), link_stream)) {
					if (link_stream->index == parent->links.size())
						parent->links.resize (1 + link_stream->index);
					parent->links[link_stream->index] = link_stream;
				} else {
					LOG(WARNING) << "Cannot create stream for \"" << link_name << "\".";
				}
			}

		default:
			break;
		}
	}

	consumer_rssl_length_ = sizeof (consumer_rssl_buf_);
	if (!WriteRaw ((rwf_major_version * 256) + rwf_minor_version,
			parent->token,
			consumer_->service_id(),
			parent->item_name,
			nullptr,
			stream->index, is_complete,
			v,
			consumer_rssl_buf_,
			&consumer_rssl_length_))
	{
		LOG(ERROR) << "WriteRaw failed";
		return false;
	}

	if (nullptr == stream->cow_handle) {
		stream->cow_handle = rsslPayloadEntryCreate (consumer_->cache_handle(), &rssl_cache_err);
		if (nullptr == stream->cow_handle) {
			LOG(ERROR) << "rsslPayloadCacheCreate: { "
				  "\"rsslErrorId\": " << rssl_cache_err.rsslErrorId << ""
				", \"text\": \"" << rssl_cache_err.text << "\""
				" }";
			return false;
		}
	}

	RsslMsg cache_msg = RSSL_INIT_MSG;
	RsslBuffer data_buffer = { static_cast<uint32_t> (consumer_rssl_length_), consumer_rssl_buf_ };

	rc = rsslSetDecodeIteratorBuffer (&it, &data_buffer);
	if (RSSL_RET_SUCCESS != rc) {
		LOG(ERROR) << "rsslSetDecodeIteratorBuffer: { "
                          "\"returnCode\": " << static_cast<signed> (rc) << ""
                        ", \"enumeration\": \"" << rsslRetCodeToString (rc) << "\""
                        ", \"text\": \"" << rsslRetCodeInfo (rc) << "\""
			" }";
		return false;
	}
	rc = rsslSetDecodeIteratorRWFVersion (&it, rwf_major_version, rwf_minor_version);
	if (RSSL_RET_SUCCESS != rc) {
		LOG(ERROR) << "rsslSetDecodeIteratorRWFVersion: { "
                          "\"returnCode\": " << static_cast<signed> (rc) << ""
                        ", \"enumeration\": \"" << rsslRetCodeToString (rc) << "\""
                        ", \"text\": \"" << rsslRetCodeInfo (rc) << "\""
			" }";
		return false;
	}
	rc = rsslDecodeMsg (&it, &cache_msg);
	if (RSSL_RET_SUCCESS != rc) {
		LOG(ERROR) << "rsslDecodeMsg: { "
                          "\"returnCode\": " << static_cast<signed> (rc) << ""
                        ", \"enumeration\": \"" << rsslRetCodeToString (rc) << "\""
                        ", \"text\": \"" << rsslRetCodeInfo (rc) << "\""
			" }";
		return false;
	}
	rc = rsslPayloadEntryApply (stream->cow_handle, &it, &cache_msg, &rssl_cache_err);
	if (RSSL_RET_SUCCESS != rc) {
		LOG(ERROR) << "rsslPayloadEntryApply: { "
			  "\"rsslErrorId\": " << rssl_cache_err.rsslErrorId << ""
			", \"text\": \"" << rssl_cache_err.text << "\""
			" }";
		return false;
	}
	stream->snapshot_handle.store ((uintptr_t)(void*)stream->cow_handle);
	return true;
}

bool
chainy::chainy_t::OnRequest (
	uintptr_t handle,
	uint16_t rwf_version, 
	int32_t token,
	uint16_t service_id,
	const std::string& item_name,
	bool use_attribinfo_in_updates
	)
{
	DVLOG(3) << "Request: { "
		  "\"handle\": " << handle << ""
		", \"rwf_version\": " << rwf_version << ""
		", \"token\": " << token << ""
		", \"service_id\": " << service_id << ""
		", \"item_name\": \"" << item_name << "\""
		", \"use_attribinfo_in_updates\": " << (use_attribinfo_in_updates ? "true" : "false") << ""
		" }";
/* Reset message buffer */
	provider_rssl_length_ = sizeof (provider_rssl_buf_);
/* Validate symbol */
	auto search = streams_.find (item_name);
	if (search == streams_.end()) {
		LOG(INFO) << "Closing resource not found for \"" << item_name << "\"";
		if (!provider_t::WriteRawClose (
				rwf_version,
				token,
				service_id,
				RSSL_DMT_MARKET_PRICE,
				item_name,
				use_attribinfo_in_updates,
				RSSL_STREAM_CLOSED, RSSL_SC_NOT_FOUND, kErrorNotFound,
				provider_rssl_buf_,
				&provider_rssl_length_
				))
		{
			return false;
		}
		if (!provider_->SendReplyAndClose (reinterpret_cast<RsslChannel*> (handle), token, provider_rssl_buf_, provider_rssl_length_))
		{
			return false;
		}
	}

	unsigned part_number = 0;
	for (auto it = search->second->links.begin();
		it != search->second->links.end();
		++it)
	{
		auto stream = *it;
		bool is_complete = it == std::prev (search->second->links.end());

/* Reset message buffer */
		provider_rssl_length_ = sizeof (provider_rssl_buf_);
		if (!WriteRaw (rwf_version,
				token,
				service_id,
				item_name,
				nullptr,
				part_number++,
				is_complete,
				(RsslPayloadEntryHandle)(void*)stream->snapshot_handle.load(),
				provider_rssl_buf_,
				&provider_rssl_length_))
		{
/* Extremely unlikely situation that writing the response fails but writing a close will not */
			if (!provider_t::WriteRawClose (
					rwf_version,
					token,
					service_id,
					RSSL_DMT_MARKET_PRICE,
					item_name,
					use_attribinfo_in_updates,
					RSSL_STREAM_CLOSED_RECOVER, RSSL_SC_ERROR, kErrorInternal,
					provider_rssl_buf_,
					&provider_rssl_length_
					))
			{
				return false;
			}
		}
		if (!provider_->SendReply (reinterpret_cast<RsslChannel*> (handle), token, provider_rssl_buf_, provider_rssl_length_, is_complete))
		{
			return false;
		}
	}
	return true;
}

bool
chainy::chainy_t::WriteRaw (
	uint16_t rwf_version,
	int32_t token,
	uint16_t service_id,
	const chromium::StringPiece& item_name,
	const chromium::StringPiece& dacs_lock,		/* ignore DACS lock */
	unsigned part_number,				/* 0 indicates initial part */
	bool is_complete,				/* mark refresh-complete */
	const std::vector<std::string>& symbol_list,
	void* data,
	size_t* length
	)
{
/* 7.4.8.1 Create a response message (4.2.2) */
	RsslRefreshMsg response = RSSL_INIT_REFRESH_MSG;
#ifndef NDEBUG
	RsslEncodeIterator it = RSSL_INIT_ENCODE_ITERATOR;
#else
	RsslEncodeIterator it;
	rsslClearEncodeIterator (&it);
#endif
	RsslBuffer buf = { static_cast<uint32_t> (*length), static_cast<char*> (data) };
	RsslRet rc;

	DCHECK(!item_name.empty());

/* 7.4.8.3 Set the message model type of the response. */
	response.msgBase.domainType = RSSL_DMT_SYMBOL_LIST;
/* 7.4.8.4 Set response type, response type number, and indication mask. */
	response.msgBase.msgClass = RSSL_MC_REFRESH;
/* for snapshot images do not cache */
	response.flags = RSSL_RFMF_SOLICITED | RSSL_RFMF_HAS_PART_NUM;
	if (0 == part_number)
		response.flags |= RSSL_RFMF_CLEAR_CACHE;
	if (is_complete)
		response.flags |= RSSL_RFMF_REFRESH_COMPLETE;
	response.partNum = part_number;

/* RDM map for a symbol list. */
	response.msgBase.containerType = RSSL_DT_MAP;

/* 7.4.8.2 Create or re-use a request attribute object (4.2.4) */
	response.msgBase.msgKey.serviceId   = service_id;
	response.msgBase.msgKey.nameType    = RDM_INSTRUMENT_NAME_TYPE_RIC;
	response.msgBase.msgKey.name.data   = const_cast<char*> (item_name.data());
	response.msgBase.msgKey.name.length = static_cast<uint32_t> (item_name.size());
	response.msgBase.msgKey.flags = RSSL_MKF_HAS_SERVICE_ID | RSSL_MKF_HAS_NAME_TYPE | RSSL_MKF_HAS_NAME;
	response.flags |= RSSL_RFMF_HAS_MSG_KEY;
/* Set the request token. */
	response.msgBase.streamId = token;

/** Optional: but require to replace stale values in cache when stale values are supported. **/
/* Item interaction state: Open, Closed, ClosedRecover, Redirected, NonStreaming, or Unspecified. */
	response.state.streamState = RSSL_STREAM_NON_STREAMING;
/* Data quality state: Ok, Suspect, or Unspecified. */
	response.state.dataState = RSSL_DATA_OK;
/* Error code, e.g. NotFound, InvalidArgument, ... */
	response.state.code = RSSL_SC_NONE;

	rc = rsslSetEncodeIteratorBuffer (&it, &buf);
	if (RSSL_RET_SUCCESS != rc) {
		LOG(ERROR) << "rsslSetEncodeIteratorBuffer: { "
			  "\"returnCode\": " << static_cast<signed> (rc) << ""
			", \"enumeration\": \"" << rsslRetCodeToString (rc) << "\""
			", \"text\": \"" << rsslRetCodeInfo (rc) << "\""
			" }";
		return false;
	}
	rc = rsslSetEncodeIteratorRWFVersion (&it, provider_t::rwf_major_version (rwf_version), provider_t::rwf_major_version (rwf_version));
	if (RSSL_RET_SUCCESS != rc) {
		LOG(ERROR) << "rsslSetEncodeIteratorRWFVersion: { "
			  "\"returnCode\": " << static_cast<signed> (rc) << ""
			", \"enumeration\": \"" << rsslRetCodeToString (rc) << "\""
			", \"text\": \"" << rsslRetCodeInfo (rc) << "\""
			", \"majorVersion\": " << static_cast<unsigned> (provider_t::rwf_major_version (rwf_version)) << ""
			", \"minorVersion\": " << static_cast<unsigned> (provider_t::rwf_minor_version (rwf_version)) << ""
			" }";
		return false;
	}
	rc = rsslEncodeMsgInit (&it, reinterpret_cast<RsslMsg*> (&response), /* maximum size */ 0);
	if (RSSL_RET_ENCODE_CONTAINER != rc) {
		LOG(ERROR) << "rsslEncodeMsgInit: { "
			  "\"returnCode\": " << static_cast<signed> (rc) << ""
			", \"enumeration\": \"" << rsslRetCodeToString (rc) << "\""
			", \"text\": \"" << rsslRetCodeInfo (rc) << "\""
			" }";
		return false;
	}
/* RSSL map { RsslBuffer -> NULL } */
	RsslMap rssl_map = RSSL_INIT_MAP;
	rssl_map.containerType = RSSL_DT_NO_DATA;
	rssl_map.keyPrimitiveType = RSSL_DT_BUFFER;
	rc = rsslEncodeMapInit (&it, &rssl_map, 0 /* summary size */, 0 /* max size */);
	if (RSSL_RET_SUCCESS != rc) {
		LOG(ERROR) << "rsslEncodeMapInit: { "
			  "\"returnCode\": " << static_cast<signed> (rc) << ""
			", \"enumeration\": \"" << rsslRetCodeToString (rc) << "\""
			", \"text\": \"" << rsslRetCodeInfo (rc) << "\""
			" }";
		return false;
	}
	for (const std::string& s : symbol_list) {
		RsslMapEntry map_entry = RSSL_INIT_MAP_ENTRY;
		const RsslBuffer key_data = { static_cast<uint32_t> (s.size()), const_cast<char *> (s.data()) };
		map_entry.action = RSSL_MPEA_ADD_ENTRY;
		rc = rsslEncodeMapEntry (&it, &map_entry, &key_data);
		if (RSSL_RET_SUCCESS != rc) {
			LOG(ERROR) << "rsslEncodeMapEntry: { "
				  "\"returnCode\": " << static_cast<signed> (rc) << ""
				", \"enumeration\": \"" << rsslRetCodeToString (rc) << "\""
				", \"text\": \"" << rsslRetCodeInfo (rc) << "\""
				" }";
			return false;
		}
	}
	rc = rsslEncodeMapComplete (&it, RSSL_TRUE /* commit */);
	if (RSSL_RET_SUCCESS != rc) {
		LOG(ERROR) << "rsslEncodeMapComplete: { "
			  "\"returnCode\": " << static_cast<signed> (rc) << ""
			", \"enumeration\": \"" << rsslRetCodeToString (rc) << "\""
			", \"text\": \"" << rsslRetCodeInfo (rc) << "\""
			" }";
		return false;
	}
/* finalize multi-step encoder */
	rc = rsslEncodeMsgComplete (&it, RSSL_TRUE /* commit */);
	if (RSSL_RET_SUCCESS != rc) {
		LOG(ERROR) << "rsslEncodeMsgComplete: { "
			  "\"returnCode\": " << static_cast<signed> (rc) << ""
			", \"enumeration\": \"" << rsslRetCodeToString (rc) << "\""
			", \"text\": \"" << rsslRetCodeInfo (rc) << "\""
			" }";
		return false;
	}
	buf.length = rsslGetEncodedBufferLength (&it);
	LOG_IF(WARNING, 0 == buf.length) << "rsslGetEncodedBufferLength returned 0.";

	if (DCHECK_IS_ON()) {
/* Message validation: must use ASSERT libraries for error description :/ */
		if (!rsslValidateMsg (reinterpret_cast<RsslMsg*> (&response))) {
			LOG(ERROR) << "rsslValidateMsg failed.";
			return false;
		} else {
			DVLOG(4) << "rsslValidateMsg succeeded.";
		}
	}
	*length = static_cast<size_t> (buf.length);
	return true;
}

bool
chainy::chainy_t::WriteRaw (
	uint16_t rwf_version,
	int32_t token,
	uint16_t service_id,
	const chromium::StringPiece& item_name,
	const chromium::StringPiece& dacs_lock,		/* ignore DACS lock */
	unsigned part_number,				/* 0 indicates initial part */
	bool is_complete,				/* mark refresh-complete */
	const RsslPayloadEntryHandle payload_entry_handle,
	void* data,
	size_t* length
	)
{
/* 7.4.8.1 Create a response message (4.2.2) */
	RsslRefreshMsg response = RSSL_INIT_REFRESH_MSG;
#ifndef NDEBUG
	RsslEncodeIterator it = RSSL_INIT_ENCODE_ITERATOR;
#else
	RsslEncodeIterator it;
	rsslClearEncodeIterator (&it);
#endif
	RsslBuffer buf = { static_cast<uint32_t> (*length), static_cast<char*> (data) };
	RsslRet rc;

	DCHECK(!item_name.empty());

/* 7.4.8.3 Set the message model type of the response. */
	response.msgBase.domainType = RSSL_DMT_SYMBOL_LIST;
/* 7.4.8.4 Set response type, response type number, and indication mask. */
	response.msgBase.msgClass = RSSL_MC_REFRESH;
/* for snapshot images do not cache */
	response.flags = RSSL_RFMF_SOLICITED | RSSL_RFMF_HAS_PART_NUM;
	if (0 == part_number)   
		response.flags |= RSSL_RFMF_CLEAR_CACHE;
	if (is_complete)        
		response.flags |= RSSL_RFMF_REFRESH_COMPLETE;
	response.partNum = part_number;

/* RDM map for a symbol list. */
	response.msgBase.containerType = RSSL_DT_MAP;

/* 7.4.8.2 Create or re-use a request attribute object (4.2.4) */
	response.msgBase.msgKey.serviceId   = service_id;
	response.msgBase.msgKey.nameType    = RDM_INSTRUMENT_NAME_TYPE_RIC;
	response.msgBase.msgKey.name.data   = const_cast<char*> (item_name.data());
	response.msgBase.msgKey.name.length = static_cast<uint32_t> (item_name.size());
	response.msgBase.msgKey.flags = RSSL_MKF_HAS_SERVICE_ID | RSSL_MKF_HAS_NAME_TYPE | RSSL_MKF_HAS_NAME;
	response.flags |= RSSL_RFMF_HAS_MSG_KEY;
/* Set the request token. */
	response.msgBase.streamId = token;

/** Optional: but require to replace stale values in cache when stale values are supported. **/
/* Item interaction state: Open, Closed, ClosedRecover, Redirected, NonStreaming, or Unspecified. */
	response.state.streamState = RSSL_STREAM_NON_STREAMING;
/* Data quality state: Ok, Suspect, or Unspecified. */
	response.state.dataState = RSSL_DATA_OK;
/* Error code, e.g. NotFound, InvalidArgument, ... */
	response.state.code = RSSL_SC_NONE;

	rc = rsslSetEncodeIteratorBuffer (&it, &buf);
	if (RSSL_RET_SUCCESS != rc) {
		LOG(ERROR) << "rsslSetEncodeIteratorBuffer: { "
			  "\"returnCode\": " << static_cast<signed> (rc) << ""
			", \"enumeration\": \"" << rsslRetCodeToString (rc) << "\""
			", \"text\": \"" << rsslRetCodeInfo (rc) << "\""
			" }";
		return false;
	}
	rc = rsslSetEncodeIteratorRWFVersion (&it, provider_t::rwf_major_version (rwf_version), provider_t::rwf_major_version (rwf_version));
	if (RSSL_RET_SUCCESS != rc) {
		LOG(ERROR) << "rsslSetEncodeIteratorRWFVersion: { "
			  "\"returnCode\": " << static_cast<signed> (rc) << ""
			", \"enumeration\": \"" << rsslRetCodeToString (rc) << "\""
			", \"text\": \"" << rsslRetCodeInfo (rc) << "\""
			", \"majorVersion\": " << static_cast<unsigned> (provider_t::rwf_major_version (rwf_version)) << ""
			", \"minorVersion\": " << static_cast<unsigned> (provider_t::rwf_minor_version (rwf_version)) << ""
			" }";
		return false;
	}
	rc = rsslEncodeMsgInit (&it, reinterpret_cast<RsslMsg*> (&response), /* maximum size */ 0);
	if (RSSL_RET_ENCODE_CONTAINER != rc) {
		LOG(ERROR) << "rsslEncodeMsgInit: { "
			  "\"returnCode\": " << static_cast<signed> (rc) << ""
			", \"enumeration\": \"" << rsslRetCodeToString (rc) << "\""
			", \"text\": \"" << rsslRetCodeInfo (rc) << "\""
			" }";
		return false;
	}
/* Providing a null payload handle sometimes results in the following:
 *
 * rsslPayloadEntryRetrieve: { "rsslErrorId": -1, "text": "rsslPayloadEntryRetrieve: invalid inputs" }
 */
	if (0 != payload_entry_handle) {
		RsslCacheError rssl_cache_err;
		rsslCacheErrorClear (&rssl_cache_err);
                rc = rsslPayloadEntryRetrieve (payload_entry_handle, &it, nullptr, &rssl_cache_err);
                if (RSSL_RET_SUCCESS != rc) {
                        LOG(ERROR) << "rsslPayloadEntryRetrieve: { "
                                  "\"rsslErrorId\": " << rssl_cache_err.rsslErrorId << ""
                                ", \"text\": \"" << rssl_cache_err.text << "\""
                                " }";
                        return false;
                }
	}
/* finalize multi-step encoder */
	rc = rsslEncodeMsgComplete (&it, RSSL_TRUE /* commit */);
	if (RSSL_RET_SUCCESS != rc) {
		LOG(ERROR) << "rsslEncodeMsgComplete: { "
			  "\"returnCode\": " << static_cast<signed> (rc) << ""
			", \"enumeration\": \"" << rsslRetCodeToString (rc) << "\""
			", \"text\": \"" << rsslRetCodeInfo (rc) << "\""
			" }";
		return false;
	}
	buf.length = rsslGetEncodedBufferLength (&it);
	LOG_IF(WARNING, 0 == buf.length) << "rsslGetEncodedBufferLength returned 0.";

	if (DCHECK_IS_ON()) {
/* Message validation: must use ASSERT libraries for error description :/ */
		if (!rsslValidateMsg (reinterpret_cast<RsslMsg*> (&response))) {
			LOG(ERROR) << "rsslValidateMsg failed.";
			return false;
		} else {
			DVLOG(4) << "rsslValidateMsg succeeded.";
		}
	}
	*length = static_cast<size_t> (buf.length);
	return true;
}

bool
chainy::chainy_t::Start()
{
	LOG(INFO) << "Starting instance: { "
		" }";
	if (!shutting_down_ && Initialize()) {
/* Spawn new thread for message pump. */
		consumer_thread_.reset (new boost::thread ([this]() {
			ConsumerLoop();
/* Raise condition loop is complete. */
			boost::lock_guard<boost::mutex> lock (consumer_lock_);
			consumer_shutdown_ = true;
			consumer_cond_.notify_one();
		}));
		provider_thread_.reset (new boost::thread ([this]() {
			ProviderLoop();
			boost::lock_guard<boost::mutex> lock (provider_lock_);
			provider_shutdown_ = true;
			provider_cond_.notify_one();
		}));
	}
	return true;
}

void
chainy::chainy_t::Stop()
{
	LOG(INFO) << "Shutting down instance: { "
		" }";
	shutting_down_ = true;
	if ((bool)provider_) {
		provider_->Quit();
/* Wait for mainloop to quit */
		boost::unique_lock<boost::mutex> lock (provider_lock_);
		while (!provider_shutdown_)
			provider_cond_.wait (lock);
	}
	if ((bool)consumer_) {
		consumer_->Quit();
/* Wait for mainloop to quit */
		boost::unique_lock<boost::mutex> lock (consumer_lock_);
		while (!consumer_shutdown_)
			consumer_cond_.wait (lock);
	}
	Reset();
}

void
chainy::chainy_t::Reset()
{
/* Release everything with an UPA dependency. */
	if ((bool)consumer_)
		consumer_->Close();
	CHECK_LE (consumer_.use_count(), 1);
	if ((bool)provider_)
		provider_->Close();
	CHECK_LE (provider_.use_count(), 1);
	consumer_.reset();
	provider_.reset();
/* Final tests before releasing UPA context */
	chromium::debug::LeakTracker<client_t>::CheckForLeaks();
	chromium::debug::LeakTracker<provider_t>::CheckForLeaks();
/* No more UPA sockets so close up context */
	CHECK_LE (upa_.use_count(), 1);
	upa_.reset();
	chromium::debug::LeakTracker<upa_t>::CheckForLeaks();
}

void
chainy::chainy_t::ConsumerLoop()
{
	try {
		consumer_->Run(); 
	} catch (const std::exception& e) {
		LOG(ERROR) << "Runtime exception: { "
			"\"What\": \"" << e.what() << "\" }";
	}
}

void
chainy::chainy_t::ProviderLoop()
{
	try {
		provider_->Run(); 
	} catch (const std::exception& e) {
		LOG(ERROR) << "Runtime exception: { "
			"\"What\": \"" << e.what() << "\" }";
	}
}

/* eof */
