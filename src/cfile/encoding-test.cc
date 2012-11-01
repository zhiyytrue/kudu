// Copyright (c) 2012, Cloudera, inc.

#include <boost/foreach.hpp>
#include <boost/ptr_container/ptr_vector.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/utility/binary.hpp>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <stdlib.h>

#include "gutil/stringprintf.h"
#include "util/test_macros.h"

#include "cfile.h"
#include "block_encodings.h"

namespace kudu { namespace cfile {


class TestEncoding : public ::testing::Test {
protected:
  // Encodes the given four ints as group-varint, then
  // decodes and ensures the result is the same.
  static void DoTestRoundTripGVI32(
    uint32_t a, uint32_t b, uint32_t c, uint32_t d) {

    faststring buf;
    AppendGroupVarInt32(&buf, a, b, c, d);

    uint32_t a_rt, b_rt, c_rt, d_rt;

    const uint8_t *end = DecodeGroupVarInt32(
      reinterpret_cast<const uint8_t *>(buf.data()),
      &a_rt, &b_rt, &c_rt, &d_rt);

    ASSERT_EQ(a, a_rt);
    ASSERT_EQ(b, b_rt);
    ASSERT_EQ(c, c_rt);
    ASSERT_EQ(d, d_rt);
    ASSERT_EQ(reinterpret_cast<const char *>(end),
              buf.data() + buf.size());
  }
};


TEST_F(TestEncoding, TestGroupVarInt) {
  faststring buf;
  AppendGroupVarInt32(&buf, 0, 0, 0, 0);
  ASSERT_EQ(5UL, buf.size());
  ASSERT_EQ(0, memcmp("\x00\x00\x00\x00\x00", buf.data(), 5));
  buf.clear();

  // All 1-byte
  AppendGroupVarInt32(&buf, 1, 2, 3, 254);
  ASSERT_EQ(5UL, buf.size());
  ASSERT_EQ(0, memcmp("\x00\x01\x02\x03\xfe", buf.data(), 5));
  buf.clear();

  // Mixed 1-byte and 2-byte
  AppendGroupVarInt32(&buf, 256, 2, 3, 65535);
  ASSERT_EQ(7UL, buf.size());
  ASSERT_EQ( BOOST_BINARY( 01 00 00 01 ), buf.at(0));
  ASSERT_EQ(256, *reinterpret_cast<const uint16_t *>(&buf[1]));
  ASSERT_EQ(2, *reinterpret_cast<const uint8_t *>(&buf[3]));
  ASSERT_EQ(3, *reinterpret_cast<const uint8_t *>(&buf[4]));
  ASSERT_EQ(65535, *reinterpret_cast<const uint16_t *>(&buf[5]));
}


// Round-trip encode/decodes using group varint
TEST_F(TestEncoding, TestGroupVarIntRoundTrip) {
  // A few simple tests.
  DoTestRoundTripGVI32(0, 0, 0, 0);
  DoTestRoundTripGVI32(1, 2, 3, 4);
  DoTestRoundTripGVI32(1, 2000, 3, 200000);

  // Then a randomized test.
  for (int i = 0; i < 10000; i++) {
    DoTestRoundTripGVI32(random(), random(), random(), random());
  }
}

TEST_F(TestEncoding, TestIntBlockEncoder) {
  boost::scoped_ptr<WriterOptions> opts(new WriterOptions());
  IntBlockBuilder ibb(opts.get());

  int *ints = new int[10000];
  for (int i = 0; i < 10000; i++) {
    ints[i] = random();
  }
  ibb.Add(reinterpret_cast<int *>(ints), 10000);
  delete[] ints;

  Slice s = ibb.Finish(12345);
  LOG(INFO) << "Encoded size for 10k ints: " << s.size();

  // Test empty case -- should be 5 bytes for just the
  // header word (all zeros)
  ibb.Reset();
  s = ibb.Finish(0);
  ASSERT_EQ(5UL, s.size());
}

TEST_F(TestEncoding, TestIntBlockRoundTrip) {
  boost::scoped_ptr<WriterOptions> opts(new WriterOptions());
  const uint32_t kOrdinalPosBase = 12345;

  srand(123);

  std::vector<uint32_t> to_insert;
  for (int i = 0; i < 10003; i++) {
    to_insert.push_back(random());
  }

  IntBlockBuilder ibb(opts.get());
  ibb.Add(&to_insert[0], to_insert.size());
  Slice s = ibb.Finish(kOrdinalPosBase);

  IntBlockDecoder ibd(s);
  ibd.ParseHeader();

  ASSERT_EQ(kOrdinalPosBase, ibd.ordinal_pos());

  std::vector<uint32_t> decoded;
  decoded.resize(to_insert.size());

  int dec_count = 0;
  while (ibd.HasNext()) {
    ASSERT_EQ((uint32_t)(kOrdinalPosBase + dec_count),
              ibd.ordinal_pos());

    int to_decode = (random() % 30) + 1;
    int n = ibd.GetNextValues(to_decode, &decoded[dec_count]);
    ASSERT_GE(to_decode, n);
    dec_count += n;
  }

  ASSERT_EQ((int)to_insert.size(), dec_count);

  for (uint i = 0; i < to_insert.size(); i++) {
    if (to_insert[i] != decoded[i]) {
      FAIL() << "Fail at index " << i <<
        " inserted=" << to_insert[i] << " got=" << decoded[i];
    }
  }

  // Test Seek within block
  for (int i = 0; i < 100; i++) {
    int seek_off = random() % decoded.size();
    ibd.SeekToPositionInBlock(seek_off);

    EXPECT_EQ((uint32_t)(kOrdinalPosBase + seek_off),
              ibd.ordinal_pos());
    uint32_t ret;
    int n = ibd.GetNextValues(1, &ret);
    EXPECT_EQ(1, n);
    EXPECT_EQ(decoded[seek_off], ret);
  }
}

TEST_F(TestEncoding, TestStringBlockBuilderRoundTrip) {
  WriterOptions opts;
  boost::ptr_vector<string> to_insert;
  std::vector<Slice> slices;

  const uint kCount = 10;

  // Prepare 10K items (storage and associated slices)
  for (uint i = 0; i < kCount; i++) {
    string *val = new string(StringPrintf("hello %d", i));
    to_insert.push_back(val);
    slices.push_back(Slice(*val));
  }

  // Push into a block builder
  StringBlockBuilder sbb(&opts);

  int rem = slices.size();
  Slice *ptr = &slices[0];
  while (rem > 0) {
    int added = sbb.Add(reinterpret_cast<void *>(ptr), rem);
    CHECK(added > 0);
    rem -= added;
    ptr += added;
  }

  ASSERT_EQ(slices.size(), sbb.Count());
  Slice s = sbb.Finish(12345L);

  // the slice should take at least a few bytes per entry
  ASSERT_GT(s.size(), kCount * 2u);

  StringBlockDecoder sbd(s);
  ASSERT_STATUS_OK(sbd.ParseHeader());
  ASSERT_EQ(kCount, sbd.Count());
  ASSERT_EQ(12345u, sbd.ordinal_pos());
  ASSERT_TRUE(sbd.HasNext());

  // Iterate one by one through data, verifying that it matches
  // what we put in.
  for (uint i = 0; i < kCount; i++) {
    ASSERT_EQ(12345u + i, sbd.ordinal_pos());

    Slice s;
    ASSERT_EQ(1, sbd.GetNextValues(1, &s));
    string expected = StringPrintf("hello %d", i);
    ASSERT_EQ(expected, s.ToString());
  }
  ASSERT_FALSE(sbd.HasNext());

  // Now iterate backwards using positional seeking
  for (int i = kCount - 1; i >= 0; i--) {
    sbd.SeekToPositionInBlock(i);
    ASSERT_EQ(12345u + i, sbd.ordinal_pos());
  }

  // Try to request a bunch of data in one go
  scoped_array<Slice> decoded(new Slice[kCount]);
  sbd.SeekToPositionInBlock(0);
  int n = sbd.GetNextValues(kCount, &decoded[0]);
  ASSERT_EQ((int)kCount, n);
  ASSERT_FALSE(sbd.HasNext());

  for (uint i = 0; i < kCount; i++) {
    string expected = StringPrintf("hello %d", i);
    ASSERT_EQ(expected, decoded[i]);
  }
}


} // namespace cfile
} // namespace kudu

int main(int argc, char **argv) {
  google::InstallFailureSignalHandler();
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
