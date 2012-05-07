// Copyright 2011 Google Inc.
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

#include "mod_spdy/apache/filters/http_to_spdy_filter.h"

#include <string>

#include "httpd.h"
#include "apr_buckets.h"
#include "apr_tables.h"
#include "util_filter.h"

#include "base/memory/scoped_ptr.h"
#include "base/string_piece.h"
#include "mod_spdy/apache/pool_util.h"
#include "mod_spdy/common/protocol_util.h"
#include "mod_spdy/common/spdy_frame_priority_queue.h"
#include "mod_spdy/common/spdy_stream.h"
#include "mod_spdy/common/testing/spdy_frame_matchers.h"
#include "mod_spdy/common/version.h"
#include "net/spdy/buffered_spdy_framer.h"
#include "net/spdy/spdy_framer.h"
#include "net/spdy/spdy_protocol.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

using mod_spdy::testing::FlagFinIs;
using mod_spdy::testing::IsControlFrameOfType;
using mod_spdy::testing::IsDataFrame;
using mod_spdy::testing::IsDataFrameWith;
using mod_spdy::testing::StreamIdIs;
using mod_spdy::testing::UncompressedHeadersAre;
using testing::Pointee;

namespace {

class HttpToSpdyFilterTest : public testing::TestWithParam<int> {
 public:
  HttpToSpdyFilterTest()
      : framer_(GetParam()),
        buffered_framer_(GetParam()),
        connection_(static_cast<conn_rec*>(
          apr_pcalloc(local_.pool(), sizeof(conn_rec)))),
        request_(static_cast<request_rec*>(
            apr_pcalloc(local_.pool(), sizeof(request_rec)))),
        ap_filter_(static_cast<ap_filter_t*>(
            apr_pcalloc(local_.pool(), sizeof(ap_filter_t)))),
        bucket_alloc_(apr_bucket_alloc_create(local_.pool())),
        brigade_(apr_brigade_create(local_.pool(), bucket_alloc_)) {
    // Set up our Apache data structures.  To keep things simple, we set only
    // the bare minimum of necessary fields, and rely on apr_pcalloc to zero
    // all others.
    connection_->pool = local_.pool();
    request_->pool = local_.pool();
    request_->connection = connection_;
    request_->protocol = const_cast<char*>("HTTP/1.1");
    request_->status = 200;
    request_->headers_out = apr_table_make(local_.pool(), 3);
    ap_filter_->c = connection_;
    ap_filter_->r = request_;
  }

 protected:
  void AddHeapBucket(base::StringPiece str) {
    APR_BRIGADE_INSERT_TAIL(brigade_, apr_bucket_heap_create(
        str.data(), str.size(), NULL, bucket_alloc_));
  }

  void AddImmortalBucket(base::StringPiece str) {
    APR_BRIGADE_INSERT_TAIL(brigade_, apr_bucket_immortal_create(
        str.data(), str.size(), bucket_alloc_));
  }

  void AddFlushBucket() {
    APR_BRIGADE_INSERT_TAIL(brigade_, apr_bucket_flush_create(bucket_alloc_));
  }

  void AddEosBucket() {
    APR_BRIGADE_INSERT_TAIL(brigade_, apr_bucket_eos_create(bucket_alloc_));
  }

  apr_status_t WriteBrigade(mod_spdy::HttpToSpdyFilter* filter) {
    return filter->Write(ap_filter_, brigade_);
  }

  void ExpectSynReply(net::SpdyStreamId stream_id,
                      const net::SpdyHeaderBlock& headers,
                      bool flag_fin) {
    net::SpdyFrame* raw_frame = NULL;
    ASSERT_TRUE(output_queue_.Pop(&raw_frame));
    ASSERT_TRUE(raw_frame != NULL);
    scoped_ptr<net::SpdyFrame> frame(raw_frame);
    EXPECT_THAT(*frame, AllOf(IsControlFrameOfType(net::SYN_REPLY),
                              StreamIdIs(stream_id),
                              UncompressedHeadersAre(headers),
                              FlagFinIs(flag_fin)));
  }

  void ExpectDataFrame(net::SpdyStreamId stream_id, base::StringPiece data,
                       bool flag_fin) {
    net::SpdyFrame* raw_frame = NULL;
    ASSERT_TRUE(output_queue_.Pop(&raw_frame));
    ASSERT_TRUE(raw_frame != NULL);
    scoped_ptr<net::SpdyFrame> frame(raw_frame);
    EXPECT_THAT(*frame, AllOf(IsDataFrameWith(data), StreamIdIs(stream_id),
                              FlagFinIs(flag_fin)));
  }

  void ExpectOutputQueueEmpty() {
    net::SpdyFrame* frame;
    EXPECT_FALSE(output_queue_.Pop(&frame));
  }

  const char* status_header_name() const {
    return (framer_.protocol_version() < 3 ? mod_spdy::spdy::kSpdy2Status :
            mod_spdy::spdy::kSpdy3Status);
  }

  const char* version_header_name() const {
    return (framer_.protocol_version() < 3 ? mod_spdy::spdy::kSpdy2Version :
            mod_spdy::spdy::kSpdy3Version);
  }

  net::SpdyFramer framer_;
  net::BufferedSpdyFramer buffered_framer_;
  mod_spdy::SpdyFramePriorityQueue output_queue_;
  mod_spdy::LocalPool local_;
  conn_rec* const connection_;
  request_rec* const request_;
  ap_filter_t* const ap_filter_;
  apr_bucket_alloc_t* const bucket_alloc_;
  apr_bucket_brigade* const brigade_;
};

TEST_P(HttpToSpdyFilterTest, ClientRequest) {
  // Set up our data structures that we're testing:
  const net::SpdyStreamId stream_id = 3;
  const net::SpdyStreamId associated_stream_id = 0;
  const net::SpdyPriority priority = framer_.GetHighestPriority();
  mod_spdy::SpdyStream stream(stream_id, associated_stream_id, priority,
                              net::kSpdyStreamInitialWindowSize,
                              &output_queue_, &buffered_framer_);
  mod_spdy::HttpToSpdyFilter http_to_spdy_filter(&stream);

  // Set the response headers of the request:
  apr_table_setn(request_->headers_out, "Connection", "close");
  apr_table_setn(request_->headers_out, "Content-Length", "12000");
  apr_table_setn(request_->headers_out, "Content-Type", "text/html");
  apr_table_setn(request_->headers_out, "Host", "www.example.com");

  // Send part of the header data into the filter:
  AddImmortalBucket("HTTP/1.1 200 OK\r\n"
                    "Connection: close\r\n");
  ASSERT_EQ(APR_SUCCESS, WriteBrigade(&http_to_spdy_filter));
  EXPECT_TRUE(APR_BRIGADE_EMPTY(brigade_));

  // Expect to get a single SYN_REPLY frame out with all the headers (read
  // from request->headers_out rather than the data we put in):
  net::SpdyHeaderBlock expected_headers;
  expected_headers[mod_spdy::http::kContentLength] = "12000";
  expected_headers[mod_spdy::http::kContentType] = "text/html";
  expected_headers[mod_spdy::http::kHost] = "www.example.com";
  expected_headers[status_header_name()] = "200";
  expected_headers[version_header_name()] = "HTTP/1.1";
  expected_headers[mod_spdy::http::kXModSpdy] =
      MOD_SPDY_VERSION_STRING "-" LASTCHANGE_STRING;
  ExpectSynReply(stream_id, expected_headers, false);
  ExpectOutputQueueEmpty();

  // Send the rest of the header data into the filter:
  AddImmortalBucket("Content-Length: 12000\r\n"
                    "Content-Type: text/html\r\n"
                    "Host: www.example.com\r\n"
                    "\r\n");
  ASSERT_EQ(APR_SUCCESS, WriteBrigade(&http_to_spdy_filter));
  EXPECT_TRUE(APR_BRIGADE_EMPTY(brigade_));

  // Expect to get nothing more out yet:
  ExpectOutputQueueEmpty();

  // Now send in some body data, with a FLUSH bucket:
  request_->sent_bodyct = 1;
  AddHeapBucket(std::string(1000, 'a'));
  AddFlushBucket();
  ASSERT_EQ(APR_SUCCESS, WriteBrigade(&http_to_spdy_filter));
  EXPECT_TRUE(APR_BRIGADE_EMPTY(brigade_));

  // Expect to get a single data frame out, containing the data we just sent:
  ExpectDataFrame(stream_id, std::string(1000, 'a'), false);
  ExpectOutputQueueEmpty();

  // Send in some more body data, this time with no FLUSH bucket:
  AddHeapBucket(std::string(2000, 'b'));
  ASSERT_EQ(APR_SUCCESS, WriteBrigade(&http_to_spdy_filter));
  EXPECT_TRUE(APR_BRIGADE_EMPTY(brigade_));

  // Expect to get nothing more out yet (because there's too little data to be
  // worth sending a frame):
  ExpectOutputQueueEmpty();

  // Send lots more body data, again with a FLUSH bucket:
  AddHeapBucket(std::string(3000, 'c'));
  AddFlushBucket();
  ASSERT_EQ(APR_SUCCESS, WriteBrigade(&http_to_spdy_filter));
  EXPECT_TRUE(APR_BRIGADE_EMPTY(brigade_));

  // This time, we should get two data frames out.
  ExpectDataFrame(stream_id, std::string(2000, 'b') + std::string(2096, 'c'),
                  false);
  ExpectDataFrame(stream_id, std::string(904, 'c'), false);
  ExpectOutputQueueEmpty();

  // Finally, send a bunch more data, followed by an EOS bucket:
  AddHeapBucket(std::string(6000, 'd'));
  AddEosBucket();
  ASSERT_EQ(APR_SUCCESS, WriteBrigade(&http_to_spdy_filter));
  EXPECT_TRUE(APR_BRIGADE_EMPTY(brigade_));

  // We should get two last data frames, the latter having FLAG_FIN set.
  ExpectDataFrame(stream_id, std::string(4096, 'd'), false);
  ExpectDataFrame(stream_id, std::string(1904, 'd'), true);
  ExpectOutputQueueEmpty();
}

// Test that the filter accepts empty brigades.
TEST_P(HttpToSpdyFilterTest, AcceptEmptyBrigade) {
  // Set up our data structures that we're testing:
  const net::SpdyStreamId stream_id = 5;
  const net::SpdyStreamId associated_stream_id = 0;
  const net::SpdyPriority priority = framer_.GetHighestPriority();
  mod_spdy::SpdyStream stream(stream_id, associated_stream_id, priority,
                              net::kSpdyStreamInitialWindowSize,
                              &output_queue_, &buffered_framer_);
  mod_spdy::HttpToSpdyFilter http_to_spdy_filter(&stream);

  // Set the response headers of the request:
  apr_table_setn(request_->headers_out, "Content-Length", "6");
  apr_table_setn(request_->headers_out, "Content-Type", "text/plain");

  // Send the header data into the filter:
  AddImmortalBucket("HTTP/1.1 200 OK\r\n"
                    "Content-Length: 6\r\n"
                    "Content-Type: text/plain\r\n"
                    "\r\n");
  ASSERT_EQ(APR_SUCCESS, WriteBrigade(&http_to_spdy_filter));
  net::SpdyHeaderBlock expected_headers;
  expected_headers[mod_spdy::http::kContentLength] = "6";
  expected_headers[mod_spdy::http::kContentType] = "text/plain";
  expected_headers[status_header_name()] = "200";
  expected_headers[version_header_name()] = "HTTP/1.1";
  expected_headers[mod_spdy::http::kXModSpdy] =
      MOD_SPDY_VERSION_STRING "-" LASTCHANGE_STRING;
  ExpectSynReply(stream_id, expected_headers, false);
  ExpectOutputQueueEmpty();

  // Send in some body data:
  request_->sent_bodyct = 1;
  AddImmortalBucket("foo");
  ASSERT_EQ(APR_SUCCESS, WriteBrigade(&http_to_spdy_filter));
  EXPECT_TRUE(APR_BRIGADE_EMPTY(brigade_));
  ExpectOutputQueueEmpty();

  // Run the filter again, with an empty brigade.  It should accept it and do
  // nothing.
  ASSERT_EQ(APR_SUCCESS, WriteBrigade(&http_to_spdy_filter));
  EXPECT_TRUE(APR_BRIGADE_EMPTY(brigade_));
  ExpectOutputQueueEmpty();

  // Send in the rest of the body data.
  request_->sent_bodyct = 1;
  AddImmortalBucket("bar");
  AddEosBucket();
  ASSERT_EQ(APR_SUCCESS, WriteBrigade(&http_to_spdy_filter));
  EXPECT_TRUE(APR_BRIGADE_EMPTY(brigade_));
  ExpectDataFrame(stream_id, "foobar", true);
  ExpectOutputQueueEmpty();
}

// Test that the filter behaves correctly when a stream is aborted halfway
// through producing output.
TEST_P(HttpToSpdyFilterTest, StreamAbort) {
  // Set up our data structures that we're testing:
  const net::SpdyStreamId stream_id = 7;
  const net::SpdyStreamId associated_stream_id = 0;
  const net::SpdyPriority priority = framer_.GetLowestPriority();
  mod_spdy::SpdyStream stream(stream_id, associated_stream_id, priority,
                              net::kSpdyStreamInitialWindowSize,
                              &output_queue_, &buffered_framer_);
  mod_spdy::HttpToSpdyFilter http_to_spdy_filter(&stream);

  // Set the response headers of the request:
  apr_table_setn(request_->headers_out, "Content-Length", "6");
  apr_table_setn(request_->headers_out, "Content-Type", "text/plain");

  // Send the header data into the filter:
  AddImmortalBucket("HTTP/1.1 200 OK\r\n"
                    "Content-Length: 6\r\n"
                    "Content-Type: text/plain\r\n"
                    "\r\n");
  ASSERT_EQ(APR_SUCCESS, WriteBrigade(&http_to_spdy_filter));
  net::SpdyHeaderBlock expected_headers;
  expected_headers[mod_spdy::http::kContentLength] = "6";
  expected_headers[mod_spdy::http::kContentType] = "text/plain";
  expected_headers[status_header_name()] = "200";
  expected_headers[version_header_name()] = "HTTP/1.1";
  expected_headers[mod_spdy::http::kXModSpdy] =
      MOD_SPDY_VERSION_STRING "-" LASTCHANGE_STRING;
  ExpectSynReply(stream_id, expected_headers, false);
  ExpectOutputQueueEmpty();

  // Send in some body data:
  request_->sent_bodyct = 1;
  AddImmortalBucket("foo");
  ASSERT_EQ(APR_SUCCESS, WriteBrigade(&http_to_spdy_filter));
  EXPECT_TRUE(APR_BRIGADE_EMPTY(brigade_));
  ExpectOutputQueueEmpty();

  // Abort the stream, and then try to send in more data.  We should get back
  // ECONNABORTED, the brigade should remain unconsumed, and the connection
  // should be marked as aborted.
  stream.AbortSilently();
  AddImmortalBucket("bar");
  AddEosBucket();
  ASSERT_FALSE(connection_->aborted);
  ASSERT_TRUE(APR_STATUS_IS_ECONNABORTED(WriteBrigade(&http_to_spdy_filter)));
  EXPECT_FALSE(APR_BRIGADE_EMPTY(brigade_));
  ExpectOutputQueueEmpty();
  ASSERT_TRUE(connection_->aborted);
}

// Run each test over both SPDY v2 and SPDY v3.
INSTANTIATE_TEST_CASE_P(Spdy2And3, HttpToSpdyFilterTest,
                        testing::Values(2, 3));

}  // namespace
