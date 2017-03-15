// Copyright (c) YugaByte, Inc.

#ifndef YB_DOCDB_DOCDB_H_
#define YB_DOCDB_DOCDB_H_

#include <cstdint>
#include <map>
#include <ostream>
#include <string>
#include <vector>

#include "rocksdb/db.h"

#include "yb/common/hybrid_time.h"
#include "yb/docdb/doc_operation.h"
#include "yb/docdb/doc_key.h"
#include "yb/docdb/doc_kv_util.h"
#include "yb/docdb/doc_path.h"
#include "yb/docdb/doc_write_batch_cache.h"
#include "yb/docdb/docdb.pb.h"
#include "yb/docdb/internal_doc_iterator.h"
#include "yb/docdb/primitive_value.h"
#include "yb/docdb/value.h"
#include "yb/tablet/mvcc.h"
#include "yb/util/shared_lock_manager.h"
#include "yb/util/status.h"

// Document DB mapping on top of the key-value map in RocksDB:
// <document_key> <hybrid_time> -> <doc_type>
// <document_key> <hybrid_time> <key_a> <gen_ts_a> -> <subdoc_a_type_or_value>
//
// Assuming the type of subdocument corresponding to key_a in the above example is "object", the
// contents of that subdocument are stored in a similar way:
// <document_key> <hybrid_time> <key_a> <gen_ts_a> <key_aa> <gen_ts_aa> -> <subdoc_aa_type_or_value>
// <document_key> <hybrid_time> <key_a> <gen_ts_a> <key_ab> <gen_ts_ab> -> <subdoc_ab_type_or_value>
// ...
//
// See doc_key.h for the encoding of the <document_key> part.
//
// <key_a>, <key_aa> are subkeys indicating a path inside a document.
// Their encoding is as follows:
//   <value_type> -- one byte, see the ValueType enum.
//   <value_specific_encoding> -- e.g. a big-endian 8-byte integer, or a string in a "zero encoded"
//                                format. This is empty for null or true/false values.
//
// <hybrid_time>, <gen_ts_a>, <gen_ts_ab> are "generation hybrid_times" corresponding to hybrid
// clock hybrid_times of the last time a particular top-level document / subdocument was fully
// overwritten or deleted.
//
// <subdoc_a_type_or_value>, <subdoc_aa_type_or_value>, <subdoc_ab_type_or_value> are values of the
// following form:
//   - One-byte value type (see the ValueType enum below).
//   - For primitive values, the encoded value. Note: the value encoding may be different from the
//     key encoding for the same data type. E.g. we only flip the sign bit for signed 64-bit
//     integers when encoded as part of a RocksDB key, not value.
//
// Also see this document for a high-level overview of how we lay out JSON documents on top of
// RocksDB:
// https://docs.google.com/document/d/1uEOHUqGBVkijw_CGD568FMt8UOJdHtiE3JROUOppYBU/edit

namespace yb {
namespace docdb {

enum class InitMarkerBehavior {
  kRequired = 0,
  kOptional = 1
};

// The DocWriteBatch class is used to build a RocksDB write batch for a DocDB batch of operations
// that may include a mix or write (set) or delete operations. It may read from RocksDB while
// writing, and builds up an internal rocksdb::WriteBatch while handling the operations.
// When all the operations are applied, the rocksdb::WriteBatch should be taken as output.
// Take ownership of it using std::move if it needs to live longer than this DocWriteBatch.
class DocWriteBatch {
 public:
  explicit DocWriteBatch(rocksdb::DB* rocksdb);

  // Set the primitive at the given path to the given value. Intermediate subdocuments are created
  // if necessary and possible.
  CHECKED_STATUS SetPrimitive(
      const DocPath& doc_path, const Value& value, HybridTime hybrid_time = HybridTime::kMax,
      InitMarkerBehavior use_init_marker = InitMarkerBehavior::kRequired);

  CHECKED_STATUS SetPrimitive(const DocPath& doc_path,
                              const PrimitiveValue& value,
                              HybridTime hybrid_time = HybridTime::kMax,
                              InitMarkerBehavior use_init_marker = InitMarkerBehavior::kRequired) {
    return SetPrimitive(doc_path, Value(value), hybrid_time, use_init_marker);
  }

  // Extend the SubDocument in the given key. We'll support List with Append and Prepend mode later.
  // TODO(akashnil): 03/20/17 ENG-1107
  // In each SetPrimitive call, some common work is repeated. It may be made more
  // efficient by not calling SetPrimitive internally.
  CHECKED_STATUS ExtendSubDocument(
      const DocPath& doc_path,
      const SubDocument& value,
      HybridTime hybrid_time = HybridTime::kMax,
      InitMarkerBehavior use_init_marker = InitMarkerBehavior::kOptional,
      MonoDelta ttl = Value::kMaxTtl);

  CHECKED_STATUS InsertSubDocument(
      const DocPath& doc_path,
      const SubDocument& value,
      HybridTime hybrid_time = HybridTime::kMax,
      InitMarkerBehavior use_init_marker = InitMarkerBehavior::kOptional,
      MonoDelta ttl = Value::kMaxTtl);

  CHECKED_STATUS DeleteSubDoc(const DocPath& doc_path, HybridTime hybrid_time = HybridTime::kMax,
                              InitMarkerBehavior use_init_marker = InitMarkerBehavior::kRequired);

  std::string ToDebugString();
  void Clear();
  bool IsEmpty() const { return put_batch_.empty(); }

  void PopulateRocksDBWriteBatchInTest(
      rocksdb::WriteBatch *rocksdb_write_batch,
      HybridTime hybrid_time = HybridTime::kMax) const;

  rocksdb::Status WriteToRocksDBInTest(
      const HybridTime hybrid_time, const rocksdb::WriteOptions &write_options) const;

  void MoveToWriteBatchPB(KeyValueWriteBatchPB *kv_pb);

  // This is used in tests when measuring the number of seeks that a given update to this batch
  // performs. The internal seek count is reset.
  int GetAndResetNumRocksDBSeeks();

  // This is used in tests to verify we are not trying to apply a DocWriteBatch to a different
  // RocksDB instance than it was constructed with.
  void CheckBelongsToSameRocksDB(const rocksdb::DB* rocksdb) const;

  rocksdb::DB* rocksdb() { return rocksdb_; }

 private:
  // This method performs the necessary operations to set a primitive value for a given docpath
  // assuming the appropriate operations have been taken care of for subkeys with index <
  // subkey_index. This method assumes responsibility of ensuring the proper docdb structure
  // (e.g: init markers) is maintained for subdocuments starting at the given subkey_index.
  CHECKED_STATUS SetPrimitiveInternal(const DocPath& doc_path, const Value& value,
                                      InternalDocIterator *doc_iter,
                                      const HybridTime hybrid_time, const bool is_deletion,
                                      const int num_subkeys, InitMarkerBehavior use_init_marker);
  DocWriteBatchCache cache_;

  rocksdb::DB* rocksdb_;
  std::vector<std::pair<std::string, std::string>> put_batch_;

  int num_rocksdb_seeks_;
};

// This function prepares the transaction by taking locks. The set of keys locked are returned to
// the caller via the keys_locked argument (because they need to be saved and unlocked when the
// transaction commits). A flag is also returned to indicate if any of the write operations
// requires a clean read snapshot to be taken before being applied (see DocOperation for details).
//
// Example: doc_write_ops might consist of the following operations:
// a.b = {}, a.b.c = 1, a.b.d = 2, e.d = 3
// We will generate all the lock_prefixes for the keys with lock types
// a - shared, a.b - exclusive, a - shared, a.b - shared, a.b.c - exclusive ...
// Then we will deduplicate the keys and promote shared locks to exclusive, and sort them.
// Finally, the locks taken will be in order:
// a - shared, a.b - exclusive, a.b.c - exclusive, a.b.d - exclusive, e - shared, e.d - exclusive.
// Then the sorted lock key list will be returned. (Type is not returned because it is not needed
// for unlocking)
// TODO(akashnil): If a.b is exclusive, we don't need to lock any sub-paths under it.
//
// Input: doc_write_ops
// Context: lock_manager
// Outputs: write_batch, need_read_snapshot
void PrepareDocWriteTransaction(const std::vector<std::unique_ptr<DocOperation>>& doc_write_ops,
                                util::SharedLockManager *lock_manager,
                                vector<string> *keys_locked,
                                bool *need_read_snapshot);

// This function reads from rocksdb and constructs the write batch.
//
// Input: doc_write_ops, read snapshot hybrid_time if requested in PrepareDocWriteTransaction().
// Context: rocksdb
// Outputs: keys_locked, write_batch
Status ApplyDocWriteTransaction(const std::vector<std::unique_ptr<DocOperation>>& doc_write_ops,
                                const HybridTime& hybrid_time,
                                rocksdb::DB *rocksdb,
                                KeyValueWriteBatchPB* write_batch);

Status HandleRedisReadTransaction(rocksdb::DB *rocksdb,
    const std::vector<std::unique_ptr<RedisReadOperation>>& doc_read_ops,
    HybridTime hybrid_time);

// A visitor class that could be overridden to consume results of scanning SubDocuments.
// See e.g. SubDocumentBuildingVisitor (used in implementing GetSubDocument) as example usage.
// We can scan any SubDocument from a node in the document tree.
class DocVisitor {
 public:
  DocVisitor() {}
  virtual ~DocVisitor() {}

  // Called once in the beginning of every new subdocument.
  virtual CHECKED_STATUS StartSubDocument(const SubDocKey &key) = 0;

  // Called in the end of a document.
  virtual CHECKED_STATUS EndSubDocument() = 0;

  // VisitKey and VisitValue are called as part of enumerating key-value pairs in an object, e.g.
  // VisitKey(key1), VisitValue(value1), VisitKey(key2), VisitValue(value2), etc.

  virtual CHECKED_STATUS VisitKey(const PrimitiveValue& key) = 0;
  virtual CHECKED_STATUS VisitValue(const PrimitiveValue& value) = 0;

  // Called in the beginning of an object, before any key/value pairs.
  virtual CHECKED_STATUS StartObject() = 0;

  // Called after all key/value pairs in an object.
  virtual CHECKED_STATUS EndObject() = 0;

  // Called before enumerating elements of an array. Not used as of 9/26/2016.
  virtual CHECKED_STATUS StartArray() = 0;

  // Called after enumerating elements of an array. Not used as of 9/26/2016.
  virtual CHECKED_STATUS EndArray() = 0;
};

// Note: subdocument_key should be an encoded SubDocument without the hybrid_time.
yb::Status ScanSubDocument(rocksdb::DB *rocksdb,
    const KeyBytes &subdocument_key,
    DocVisitor *visitor,
    HybridTime scan_ts = HybridTime::kMax);

// Returns the whole SubDocument below some node identified by subdocument_key.
// This function works with or without object init markers present.
// If tombstone and other values are inserted at the same timestamp, it results in undefined
// behavior. TODO: We should have write-id's to make sure timestamps are always unique.
yb::Status GetSubDocument(rocksdb::DB *rocksdb,
    const SubDocKey& subdocument_key,
    SubDocument *result,
    bool *doc_found,
    HybridTime scan_ts = HybridTime::kMax,
    MonoDelta table_ttl = Value::kMaxTtl);

// Create a debug dump of the document database. Tries to decode all keys/values despite failures.
// Reports all errors to the output stream and returns the status of the first failed operation,
// if any.
yb::Status DocDBDebugDump(rocksdb::DB* rocksdb, std::ostream& out, bool include_binary = false);

std::string DocDBDebugDumpToStr(rocksdb::DB* rocksdb, bool include_binary = false);

void ConfigureDocDBRocksDBOptions(rocksdb::Options* options);

}  // namespace docdb
}  // namespace yb

#endif  // YB_DOCDB_DOCDB_H_
