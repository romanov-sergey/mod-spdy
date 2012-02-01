// Copyright 2010 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef MOD_SPDY_SPDY_TO_HTTP_CONVERTER_H_
#define MOD_SPDY_SPDY_TO_HTTP_CONVERTER_H_

#include "base/basictypes.h"
#include "net/spdy/spdy_framer.h"
#include "net/spdy/spdy_protocol.h"

namespace mod_spdy {

class HttpStreamVisitorInterface;

// Incrementally converts SPDY frames to HTTP streams, and passes the HTTP
// stream to the specified HttpStreamVisitorInterface.
class SpdyToHttpConverter {
 public:
  explicit SpdyToHttpConverter(HttpStreamVisitorInterface* visitor);
  ~SpdyToHttpConverter();

  enum Status {
    SPDY_CONVERTER_SUCCESS,
    FRAME_BEFORE_SYN_STREAM,  // first frame was not a SYN_STREAM
    FRAME_AFTER_FIN,  // received another frame after a FLAG_FIN
    EXTRA_SYN_STREAM,  // received an additional SYN_STREAM after the first
    INVALID_HEADER_BLOCK,  // the headers could not be parsed
    BAD_REQUEST  // the headers didn't constitute a valid HTTP request
  };

  static const char* StatusString(Status status);

  // Convert the SPDY frame to HTTP and make appropriate calls to the visitor.
  // In some cases data may be buffered, but everything will get flushed out to
  // the visitor by the time the final frame (with FLAG_FIN set) is done.
  Status ConvertSynStreamFrame(const spdy::SpdySynStreamControlFrame& frame);
  Status ConvertHeadersFrame(const spdy::SpdyHeadersControlFrame& frame);
  Status ConvertDataFrame(const spdy::SpdyDataFrame& frame);

private:
  // Called when we see a FLAG_FIN.  This terminates the request and appends
  // whatever trailing headers (if any) we have buffered.
  void FinishRequest();

  enum State {
    NO_FRAMES_YET,        // We haven't seen any frames yet.
    RECEIVED_SYN_STREAM,  // We've seen the SYN_STREAM, but no DATA yet.
    RECEIVED_DATA,        // We've seen at least one DATA frame.
    RECEIVED_FLAG_FIN     // We've seen the FLAG_FIN; no more frames allowed.
  };

  HttpStreamVisitorInterface* const visitor_;
  spdy::SpdyHeaderBlock trailing_headers_;
  State state_;

  DISALLOW_COPY_AND_ASSIGN(SpdyToHttpConverter);
};

}  // namespace mod_spdy

#endif  // MOD_SPDY_SPDY_TO_HTTP_CONVERTER_H_