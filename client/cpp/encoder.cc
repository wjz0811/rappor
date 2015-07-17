// Copyright 2015 Google Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cassert>  // assert
#include <stdio.h>
#include <stdarg.h>  // va_list, etc.

#include "encoder.h"

namespace rappor {

void log(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);
  fprintf(stderr, "\n");
}

static int kMaxBits = sizeof(Bits) * 8;

Encoder::Encoder(
    const rappor::Params& params,
    int cohort, Md5Func* md5_func,
    const std::string& client_secret, HmacFunc* hmac_func,
    const IrrRandInterface& irr_rand)
    : num_bits_(params.num_bits),
      num_hashes_(params.num_hashes),
      prob_f_(params.prob_f),
      cohort_(cohort),
      md5_func_(md5_func),
      client_secret_(client_secret),
      hmac_func_(hmac_func),
      irr_rand_(irr_rand),

      num_bytes_(0),
      is_valid_(true) {

  // Validity constraints:
  //
  // bits fit in an integral type uint64_t:
  //   num_bits < 64 (or sizeof(Bits) * 8)
  // md5 is long enough:
  //   128 > ( num_hashes * log2(num_bits) )
  // sha256 is long enough:
  //   256 > num_bits + (prob_f resolution * num_bits)

  //num_bits_ = params.num_bits;
  //num_hashes_ = params.num_hashes;
  //prob_f_ = params.prob_f;

  //log("num_bits: %d", num_bits_);
  if (num_bits_ > kMaxBits) {
    log("num_bits (%d) can't be bigger than rappor::Bits type: (%d)",
        num_bits_, kMaxBits);
    is_valid_ = false;
  }

  //log("Mask: %016x", debug_mask_);

  // number of bytes in bloom filter
  if (num_bits_ % 8 == 0) {
    num_bytes_ = num_bits_ / 8;
    //log("num bytes: %d", num_bytes_);
  } else {
    is_valid_ = false;
  }
}

Encoder::Encoder(const Params& params, const Deps& deps) 
    : num_bits_(params.num_bits),
      num_hashes_(params.num_hashes),
      prob_f_(params.prob_f),
      cohort_(deps.cohort_),
      md5_func_(deps.md5_func_),
      client_secret_(deps.client_secret_),
      hmac_func_(deps.hmac_func_),
      irr_rand_(deps.irr_rand_) {
}

bool Encoder::IsValid() const {
  return is_valid_;
}

void PrintMd5(Md5Digest md5) {
  // GAH!  sizeof(md5) does NOT work.  Because that's a pointer.
  for (size_t i = 0; i < sizeof(Md5Digest); ++i) {
    //printf("[%d]\n", i);
    fprintf(stderr, "%02x", md5[i]);
  }
  fprintf(stderr, "\n");
}

void PrintSha256(Sha256Digest h) {
  // GAH!  sizeof(md5) does NOT work.  Because that's a pointer.
  for (size_t i = 0; i < sizeof(Sha256Digest); ++i) {
    //printf("[%d]\n", i);
    fprintf(stderr, "%02x", h[i]);
  }
  fprintf(stderr, "\n");
}

Bits Encoder::MakeBloomFilter(const std::string& value) const {
  Bits bloom = 0;

  // First do hashing.
  Md5Digest md5;
  // 4 byte cohort + actual value
  std::string hash_input(4 + value.size(), '\0');

  // Assuming num_cohorts <= 256 , the big endian representation looks like
  // [0 0 0 <cohort>]
  unsigned char c = cohort_ & 0xFF;
  hash_input[0] = '\0';
  hash_input[1] = '\0';
  hash_input[2] = '\0';
  hash_input[3] = c;

  // Copy the rest
  for (size_t i = 0; i < value.size(); ++i) {
    hash_input[i + 4] = value[i];
  }

  md5_func_(hash_input, md5);

  log("MD5:");
  PrintMd5(md5);

  // To determine which bit to set in the bloom filter, use a byte of the MD5.
  for (int i = 0; i < num_hashes_; ++i) {
    int bit_to_set = md5[i] % num_bits_;
    bloom |= 1 << bit_to_set;
    //log("Hash %d, set bit %d", i, bit_to_set);
  }
  return bloom;
}

// Helper function for PRR
void Encoder::GetPrrMasks(const std::string& value, Bits* uniform_out,
                          Bits* f_mask_out) const {
  // Create HMAC(secret, value), and use its bits to construct f and uniform
  // bits.
  Sha256Digest sha256;
  hmac_func_(client_secret_, value, sha256);

  log("secret: %s word: %s sha256:", client_secret_.c_str(), value.c_str());
  PrintSha256(sha256);

  // We should have already checked this.
  assert(num_bits_ <= 32);

  uint8_t threshold128 = static_cast<uint8_t>(prob_f_ * 128);

  Bits uniform = 0;
  Bits f_mask = 0;

  for (int i = 0; i < num_bits_; ++i) {
    uint8_t byte = sha256[i];

    uint8_t u_bit = byte & 0x01;  // 1 bit of entropy
    uniform |= (u_bit << i);  // maybe set bit in mask

    uint8_t rand128 = byte >> 1;  // 7 bits of entropy
    uint8_t noise_bit = (rand128 < threshold128);
    f_mask |= (noise_bit << i);  // maybe set bit in mask
  }

  *uniform_out = uniform;
  *f_mask_out = f_mask;
}

bool Encoder::_EncodeInternal(const std::string& value, Bits* bloom_out,
    Bits* prr_out, Bits* irr_out) const {
  rappor::log("Encode '%s' cohort %d", value.c_str(), cohort_);

  Bits bloom = MakeBloomFilter(value);
  *bloom_out = bloom;

  // Compute Permanent Randomized Response (PRR).
  Bits uniform;
  Bits f_mask;
  GetPrrMasks(value, &uniform, &f_mask);

  Bits prr = (bloom & ~f_mask) | (uniform & f_mask);
  *prr_out = prr;

  // Compute Instantaneous Randomized Response (IRR).

  // NOTE: These can fail if say a read() from /dev/urandom fails.
  Bits p_bits;
  Bits q_bits;
  if (!irr_rand_.PMask(&p_bits)) {
    return false;
  }
  if (!irr_rand_.QMask(&q_bits)) {
    return false;
  }

  Bits irr = (p_bits & ~prr) | (q_bits & prr);
  *irr_out = irr;

  return true;
}

bool Encoder::Encode(const std::string& value, Bits* irr_out) const {
  Bits unused_bloom;
  Bits unused_prr;
  return _EncodeInternal(value, &unused_bloom, &unused_prr, irr_out);
}

}  // namespace rappor
