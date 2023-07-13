#include <string>
#include <unordered_map>
#include <sstream>
#include <vector>

#include "header_rewrite.h"
#include "utility.h"

#include "source/common/common/utility.h"
#include "source/common/common/logger.h"
#include "envoy/server/filter_config.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace HeaderRewriteFilter {

void fail(absl::string_view msg) {
  auto& logger = Logger::Registry::getLog(Logger::Id::tracing);
  ENVOY_LOG_TO_LOGGER(logger, error, "Failed to parse config - {}", msg);
}

HttpHeaderRewriteFilterConfig::HttpHeaderRewriteFilterConfig(
    const sample::Decoder& proto_config)
    : key_(proto_config.key()), val_(proto_config.val()) {}

HttpHeaderRewriteFilter::HttpHeaderRewriteFilter(HttpHeaderRewriteFilterConfigSharedPtr config)
    : config_(config) {
  
  const std::string header_config = headerValue();

  // split by operation (newline delimited config)
  auto operations = StringUtil::splitToken(header_config, "\n", false, true);

  // process each operation
  for (auto const& operation : operations) {
    auto tokens = StringUtil::splitToken(operation, " ");
    if (tokens.size() < Utility::MIN_NUM_ARGUMENTS) {
      fail("too few arguments provided");
      setError();
      return;
    }

    // determine if it's request/response
    if (tokens.at(0) != Utility::HTTP_REQUEST && tokens.at(0) != Utility::HTTP_RESPONSE) {
      fail("first argument must be <http-response/http-request>");
      setError();
      return;
    }
    const bool isRequest = (tokens.at(0) == Utility::HTTP_REQUEST);

    const Utility::OperationType operation_type = Utility::StringToOperationType(absl::string_view(tokens.at(1)));
    HeaderProcessorUniquePtr processor;

    switch(operation_type) {
      case Utility::OperationType::SetHeader:
        processor = std::make_unique<SetHeaderProcessor>();
        break;
      case Utility::OperationType::SetPath:
        // TODO: implement set-path operation
        ENVOY_LOG_MISC(info, "set path operation detected!");
        return;
      default:
        fail("invalid operation type");
        setError();
        return;
    }

    // parse operation
    const absl::Status status = processor->parseOperation(tokens);
    if (!status.ok()) {
      fail(status.message());
      setError();
      return;
    }

    // keep track of operations to be executed
    if (isRequest) {
      request_header_processors_.push_back(std::move(processor));
    }
  }
}

const Http::LowerCaseString HttpHeaderRewriteFilter::headerKey() const {
  return Http::LowerCaseString(config_->key());
}

const std::string HttpHeaderRewriteFilter::headerValue() const {
  return config_->val();
}

Http::FilterHeadersStatus HttpHeaderRewriteFilter::decodeHeaders(Http::RequestHeaderMap& headers, bool) {
  if (getError()) {
    ENVOY_LOG_MISC(info, "invalid config, skipping filter");
    return Http::FilterHeadersStatus::Continue;
  }

  // execute each operation
  // TODO: run this loop for the response side too once filter type is changed to Encoder/Decoder
  for (auto const& processor : request_header_processors_) {
    processor->executeOperation(headers);
  }

  return Http::FilterHeadersStatus::Continue;
}

Http::FilterDataStatus HttpHeaderRewriteFilter::decodeData(Buffer::Instance&, bool) {
  return Http::FilterDataStatus::Continue;
}

void HttpHeaderRewriteFilter::setDecoderFilterCallbacks(Http::StreamDecoderFilterCallbacks& callbacks) {
  decoder_callbacks_ = &callbacks;
}

} // namespace HeaderRewriteFilter
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy