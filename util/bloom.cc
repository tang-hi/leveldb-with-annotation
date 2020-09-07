// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/filter_policy.h"

#include "leveldb/slice.h"
#include "util/hash.h"

namespace leveldb {

namespace {
static uint32_t BloomHash(const Slice& key) {
  return Hash(key.data(), key.size(), 0xbc9f1d34);
}

class BloomFilterPolicy : public FilterPolicy {
 private:
  size_t bits_per_key_;
  size_t k_;

 public:
  explicit BloomFilterPolicy(int bits_per_key)
      : bits_per_key_(bits_per_key) {
    // We intentionally round down to reduce probing cost a little bit
    // 向下取整以减少探测次数
    k_ = static_cast<size_t>(bits_per_key * 0.69);  // 0.69 =~ ln(2)
    if (k_ < 1) k_ = 1;
    if (k_ > 30) k_ = 30;
  }

  virtual const char* Name() const {
    return "leveldb.BuiltinBloomFilter2";
  }

  virtual void CreateFilter(const Slice* keys, int n, std::string* dst) const {
    // Compute bloom filter size (in both bits and bytes)
    size_t bits = n * bits_per_key_; // 计算布隆过滤器的大小, 布隆过滤器就是一个长度为 bits 的位图

    // For small n, we can see a very high false positive rate.  Fix it
    // by enforcing a minimum bloom filter length.
    if (bits < 64) bits = 64; // 如果 n 比较小, 那么假阳性的概率就比较大, 我们通过指定一个最小的布隆过滤器长度来避免. 

    size_t bytes = (bits + 7) / 8; // 加 7 是确保 bits / 8 能够向上取整
    bits = bytes * 8;

    const size_t init_size = dst->size();
    dst->resize(init_size + bytes, 0); // resize dst, 空位置置为 0.
    dst->push_back(static_cast<char>(k_));  // Remember # of probes in filter 将探测次数 k 保存在 resize 后的 dst 的后一个字节
    // dst[0, init_size - 1]  为 dst 原有的数据; 
    // dst[init_size, init_size + bytes - 1]  用作布隆过滤器位图; 
    // dst[init_size + bytes]  保存着布隆过滤器的探测次数 k. 
    char* array = &(*dst)[init_size];
    for (int i = 0; i < n; i++) { // 针对每个 key, 将其放入布隆过滤器
      // Use double-hashing to generate a sequence of hash values.
      // See analysis in [Kirsch,Mitzenmacher 2006].
      uint32_t h = BloomHash(keys[i]); // 计算第 i 个 key 的哈希值
      const uint32_t delta = (h >> 17) | (h << 15);  // Rotate right 17 bits 将哈希值低 17 位和高 15 位交换位置
      for (size_t j = 0; j < k_; j++) {
        const uint32_t bitpos = h % bits; // 取模计算哈希值 h 在布隆过滤器中对应的位置
        array[bitpos/8] |= (1 << (bitpos % 8)); // 将布隆过滤器第 bitpos/8 个字节的倒数(从低到高)第 bitpos%8 位置为 1.
        h += delta; // 重写哈希值 h, 以探测下个布隆过滤器位置
      }
    }
  }

  virtual bool KeyMayMatch(const Slice& key, const Slice& bloom_filter) const {
    const size_t len = bloom_filter.size(); // 获取布隆过滤器长度(单位, 字节)
    if (len < 2) return false;

    const char* array = bloom_filter.data();
    const size_t bits = (len - 1) * 8; // 因为最后一个字节存的是探测次数 k, 所以要减掉

    // Use the encoded k so that we can read filters generated by
    // bloom filters created using different parameters.
    const size_t k = array[len-1]; // 从末尾字节取出布隆过滤器探测次数 k
    if (k > 30) {
      // Reserved for potentially new encodings for short bloom filters.
      // Consider it a match.
      // 大于 30 的探测次数是为更短的布隆过滤器保留的. 
      return true;
    }

    // 下面部分与 CreateFilter 中的逻辑类似, 计算 key 的哈希值, 然后探测 k 次或者发现 key 的某次哈希值对应位图位置不为 1
    uint32_t h = BloomHash(key);
    const uint32_t delta = (h >> 17) | (h << 15);  // Rotate right 17 bits
    for (size_t j = 0; j < k; j++) {
      const uint32_t bitpos = h % bits;
      if ((array[bitpos/8] & (1 << (bitpos % 8))) == 0) return false;
      h += delta;
    }
    return true;
  }
};
}

const FilterPolicy* NewBloomFilterPolicy(int bits_per_key) {
  return new BloomFilterPolicy(bits_per_key);
}

}  // namespace leveldb
