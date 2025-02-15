#include <string>
#include <unordered_map>
#include <sstream>
#include <vector>

#include "header_rewrite.h"

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
    const envoy::extensions::filters::http::HeaderRewrite& proto_config)
    : config_(proto_config.config()) {}

HttpHeaderRewriteFilter::HttpHeaderRewriteFilter(HttpHeaderRewriteFilterConfigSharedPtr config)
    : config_(config) {

  // make bool processor map
  request_set_bool_processors_ = std::make_shared<std::unordered_map<std::string, SetBoolProcessorSharedPtr>>();
  response_set_bool_processors_ = std::make_shared<std::unordered_map<std::string, SetBoolProcessorSharedPtr>>();
  
  const std::string header_config = headerConfig();

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
    HeaderProcessorUniquePtr processor = nullptr;

    auto bool_processors = isRequest ? request_set_bool_processors_ : response_set_bool_processors_;

    switch(operation_type) {
      case Utility::OperationType::SetHeader:
      {
        processor = std::make_unique<SetHeaderProcessor>(bool_processors, isRequest);
        break;
      }
      case Utility::OperationType::AppendHeader:
      {
        processor = std::make_unique<AppendHeaderProcessor>(bool_processors, isRequest);
        break;
      }
      case Utility::OperationType::SetDynMetadata:
      {
        processor = std::make_unique<SetDynamicMetadataProcessor>(bool_processors, isRequest);
        break;
      }
      case Utility::OperationType::SetPath:
      {
        if (!isRequest) {
          fail("set-path can only be on request");
          setError();
          return;
        }
        // path being set here includes the query string
        processor = std::make_unique<SetPathProcessor>(bool_processors, isRequest);
        break;
      }
      case Utility::OperationType::SetBool:
       {
          SetBoolProcessorSharedPtr processor = std::make_unique<SetBoolProcessor>(bool_processors, isRequest);
          const std::string boolName(tokens.at(2));
          const absl::Status status = processor->parseOperation(tokens, tokens.begin() + 2);

          if (!status.ok()) {
            fail(status.message());
            setError();
            return;
          }
          // make sure this boolean variable doesn't already exist in the map
          if (isRequest && request_set_bool_processors_->find(boolName) == request_set_bool_processors_->end()) {
            request_set_bool_processors_->insert({boolName, std::move(processor)});
          } else if (!isRequest && response_set_bool_processors_->find(boolName) == response_set_bool_processors_->end()) {
            response_set_bool_processors_->insert({boolName, std::move(processor)});
          } else {
            fail("redefinition of boolean variable");
            setError();
            return;
          }
          break;
        }
      default:
        fail("invalid operation type");
        setError();
        return;
    }

    // parse operation
    if (processor) {
      const absl::Status status = processor->parseOperation(tokens, tokens.begin() + 2);
      if (!status.ok()) {
        fail(status.message());
        setError();
        return;
      }

      // keep track of request/response operations to be executed
      if (isRequest) {
        request_header_processors_.push_back(std::move(processor));
      } else {
        response_header_processors_.push_back(std::move(processor));
      }
    }
  }
}

const std::string HttpHeaderRewriteFilter::headerConfig() const {
  return config_->config();
}

Http::FilterHeadersStatus HttpHeaderRewriteFilter::decodeHeaders(Http::RequestHeaderMap& headers, bool) {
  if (getError()) {
    ENVOY_LOG_MISC(info, "invalid config, skipping filter (request side)");
    return Http::FilterHeadersStatus::Continue;
  }

  // execute each operation
  Envoy::StreamInfo::StreamInfo* streamInfo = &decoder_callbacks_->streamInfo();
  for (auto const& processor : request_header_processors_) {
    const absl::Status status = processor->executeOperation(headers, streamInfo);
    if (status != absl::OkStatus()) {
      ENVOY_LOG_MISC(info, "error executing an operation on request side, skipping filter -- " + std::string(status.message()));
      return Http::FilterHeadersStatus::Continue;
    }
  }

  return Http::FilterHeadersStatus::Continue;
}

Http::FilterHeadersStatus HttpHeaderRewriteFilter::encodeHeaders(Http::ResponseHeaderMap& headers, bool) {
  if (getError()) {
    ENVOY_LOG_MISC(info, "invalid config, skipping filter (response side)");
    return Http::FilterHeadersStatus::Continue;
  }

  // execute each operation
  Envoy::StreamInfo::StreamInfo* streamInfo = &encoder_callbacks_->streamInfo();
  for (auto const& processor : response_header_processors_) {
    const absl::Status status = processor->executeOperation(headers, streamInfo);
    if (status != absl::OkStatus()) {
      ENVOY_LOG_MISC(info, "error executing an operation on response side, skipping filter -- " + std::string(status.message()));
      return Http::FilterHeadersStatus::Continue;
    }
  }

  return Http::FilterHeadersStatus::Continue;
}

Http::FilterDataStatus HttpHeaderRewriteFilter::decodeData(Buffer::Instance&, bool) {
  return Http::FilterDataStatus::Continue;
}

Http::FilterDataStatus HttpHeaderRewriteFilter::encodeData(Buffer::Instance&, bool) {
  return Http::FilterDataStatus::Continue;
}

} // namespace HeaderRewriteFilter
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
