// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "db/builder.h"

#include "db/filename.h"
#include "db/dbformat.h"
#include "db/table_cache.h"
#include "db/version_edit.h"
#include "leveldb/db.h"
#include "leveldb/env.h"
#include "leveldb/iterator.h"

namespace leveldb {

Status BuildTable(const std::string& dbname,
                  Env* env,
                  const Options& options,
                  TableCache* table_cache,
                  Iterator* iter,
                  FileMetaData* meta) {
  Status s;
  meta->file_size = 0;
  // 将迭代器移到 memtable 起始位置
  iter->SeekToFirst();

  // 为新的 Table 文件生成一个名字
  std::string fname = TableFileName(dbname, meta->number);
  if (iter->Valid()) {
    WritableFile* file;
    // 基于新文件名创建一个文件对象并保存到 file
    s = env->NewWritableFile(fname, &file);
    if (!s.ok()) {
      return s;
    }

    // 新建一个用于构造 Table 文件的 TableBuilder
    TableBuilder* builder = new TableBuilder(options, file);
    // 获取要写入的 memtable 的最小 key 并保存到 meta->smallest
    meta->smallest.DecodeFrom(iter->key());
    // 迭代 memtable，将 <key, value> 写入到 TableBuilder，并保存最大的 key 到 meta->largest
    for (; iter->Valid(); iter->Next()) {
      Slice key = iter->key();
      // 因为 memtable 基于 skiplist 是从小到大有序的，所以最后访问的那个数据项的 key 必定是最大的
      meta->largest.DecodeFrom(key);
      builder->Add(key, iter->value());
    }

    // Finish and check for builder errors
    // 将 TableBuilder 中的数据按照 table 文件的格式写入到文件，
    // table 文件构成：data blocks，filter block，metaindex block，index block
    s = builder->Finish();
    if (s.ok()) {
      // 写成功，将 table 文件大小保存到 meta->file_size
      meta->file_size = builder->FileSize();
      assert(meta->file_size > 0);
    }
    // 不管是否构造成功都释放 TableBuilder
    delete builder;

    // Finish and check for file errors
    if (s.ok()) {
      // 确保内容写入到磁盘中
      s = file->Sync();
    }
    if (s.ok()) {
      // 同步成功则关闭文件
      s = file->Close();
    }
    // 不管文件是否同步成功，都删除文件对象
    delete file;
    file = nullptr;

    if (s.ok()) {
      // Verify that the table is usable
      Iterator* it = table_cache->NewIterator(ReadOptions(),
                                              meta->number,
                                              meta->file_size);
      s = it->status();
      delete it;
    }
  }

  // Check for input iterator errors
  if (!iter->status().ok()) {
    s = iter->status();
  }

  if (s.ok() && meta->file_size > 0) {
    // Keep it
  } else {
    env->DeleteFile(fname);
  }
  return s;
}

}  // namespace leveldb
