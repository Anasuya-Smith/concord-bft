// Concord
//
// Copyright (c) 2019-2020 VMware, Inc. All Rights Reserved.
//
// This product is licensed to you under the Apache 2.0 license (the
// "License").  You may not use this product except in compliance with the
// Apache 2.0 License.
//
// This product may include a number of subcomponents with separate copyright
// notices and license terms. Your use of these subcomponents is subject to the
// terms and conditions of the subcomponent's license, as noted in the LICENSE
// file.

#pragma once

#include <assertUtils.hpp>
#include "merkle_tree_db_adapter.h"
#include "merkle_tree_block.h"
#include "endianness.hpp"
#include "sliver.hpp"
#include "sparse_merkle/base_types.h"
#include "sparse_merkle/internal_node.h"
#include "sparse_merkle/keys.h"
#include "storage/db_types.h"
#include "string.hpp"

#include <cstdint>
#include <iterator>
#include <limits>
#include <string>

#include <utility>
#include <variant>
#include <vector>

namespace concord::kvbc::v2MerkleTree {

namespace detail {

// Specifies the child type. Used when serializing BatchedInternalNode objects.
enum class BatchedInternalNodeChildType : std::uint8_t { Internal, Leaf };

inline BatchedInternalNodeChildType getInternalChildType(const concordUtils::Sliver &buf) {
  Assert(!buf.empty());

  switch (buf[0]) {
    case concord::util::toChar(BatchedInternalNodeChildType::Internal):
      return BatchedInternalNodeChildType::Internal;
    case concord::util::toChar(BatchedInternalNodeChildType::Leaf):
      return BatchedInternalNodeChildType::Leaf;
  }

  Assert(false);
  return BatchedInternalNodeChildType::Internal;
}

using BatchedInternalMaskType = std::uint32_t;

// Serialize integral types in big-endian (network) byte order so that lexicographical comparison works and we can get
// away without a custom key comparator. toBigEndianStringBuffer() does not allow bools.
template <typename T>
std::string serializeImp(T v) {
  return concordUtils::toBigEndianStringBuffer(v);
}

inline std::string serializeImp(storage::v2MerkleTree::detail::EDBKeyType type) {
  return std::string{concord::util::toChar(type)};
}

inline std::string serializeImp(storage::v2MerkleTree::detail::EKeySubtype type) {
  return std::string{concord::util::toChar(storage::v2MerkleTree::detail::EDBKeyType::Key),
                     concord::util::toChar(type)};
}

inline std::string serializeImp(storage::v2MerkleTree::detail::EBFTSubtype type) {
  return std::string{concord::util::toChar(storage::v2MerkleTree::detail::EDBKeyType::BFT),
                     concord::util::toChar(type)};
}

inline std::string serializeImp(const std::vector<std::uint8_t> &v) {
  return std::string{std::cbegin(v), std::cend(v)};
}

inline std::string serializeImp(const sparse_merkle::NibblePath &path) {
  return serializeImp(static_cast<std::uint8_t>(path.length())) + serializeImp(path.data());
}

inline std::string serializeImp(const sparse_merkle::Hash &hash) {
  return std::string{reinterpret_cast<const char *>(hash.data()), hash.size()};
}

inline std::string serializeImp(const sparse_merkle::InternalNodeKey &key) {
  return serializeImp(key.version().value()) + serializeImp(key.path());
}

inline std::string serializeImp(const sparse_merkle::LeafKey &key) {
  return serializeImp(key.hash()) + serializeImp(key.version().value());
}

inline std::string serializeImp(const sparse_merkle::LeafChild &child) {
  return serializeImp(child.hash) + serializeImp(child.key);
}

inline std::string serializeImp(const sparse_merkle::InternalChild &child) {
  return serializeImp(child.hash) + serializeImp(child.version.value());
}

inline std::string serializeImp(const sparse_merkle::BatchedInternalNode &intNode) {
  static_assert(sparse_merkle::BatchedInternalNode::MAX_CHILDREN < 32);

  struct Visitor {
    Visitor(std::string &buf) : buf_{buf} {}

    void operator()(const sparse_merkle::LeafChild &leaf) {
      buf_ += concord::util::toChar(BatchedInternalNodeChildType::Leaf) + serializeImp(leaf);
    }

    void operator()(const sparse_merkle::InternalChild &internal) {
      buf_ += concord::util::toChar(BatchedInternalNodeChildType::Internal) + serializeImp(internal);
    }

    std::string &buf_;
  };

  const auto &children = intNode.children();
  std::string serializedChildren;
  auto mask = BatchedInternalMaskType{0};
  for (auto i = 0u; i < children.size(); ++i) {
    const auto &child = children[i];
    if (child) {
      mask |= (1 << i);
      std::visit(Visitor{serializedChildren}, *child);
    }
  }

  // If the node is empty, serialize it as an empty string.
  if (!mask) {
    return std::string{};
  }

  // Serialize by putting a bitmask (of type BatchedInternalMaskType) specifying which indexes are set in the children
  // array. Then, put a type before each child and then the child itself.
  return serializeImp(mask) + serializedChildren;
}

inline std::string serializeImp(const block::detail::Node &node) {
  const auto blockIdBuf = concordUtils::toBigEndianStringBuffer(node.blockId);

  auto nodeSize = block::detail::Node::MIN_SIZE;
  for (const auto &key : node.keys) {
    Assert(key.first.length() <= std::numeric_limits<block::detail::KeyLengthType>::max());
    nodeSize += (sizeof(block::detail::KeyLengthType) + key.first.length() + 1);  // Add a byte of key data.
  }

  std::string buf;
  buf.reserve(nodeSize);

  // Block ID.
  buf += blockIdBuf;

  // Parent digest.
  buf.append(reinterpret_cast<const char *>(node.parentDigest.data()), node.parentDigest.size());

  // State hash.
  buf.append(reinterpret_cast<const char *>(node.stateHash.data()), node.stateHash.size());

  // State root version.
  buf.append(concordUtils::toBigEndianStringBuffer(node.stateRootVersion.value()));

  // Keys in [key data, length, key] encoding.
  for (const auto &keyData : node.keys) {
    // At the moment, only the delete flag is supported. Additional data can be packed in this single byte in the
    // future.
    buf += keyData.second.deleted ? char{1} : char{0};
    buf += concordUtils::toBigEndianStringBuffer(static_cast<block::detail::KeyLengthType>(keyData.first.length()));
    buf.append(keyData.first.data(), keyData.first.length());
  }

  return buf;
}

inline std::string serializeImp(block::detail::RawBlockMerkleData data) {
  auto dataSize = block::detail::RawBlockMerkleData::MIN_SIZE;
  for (const auto &key : data.deletedKeys) {
    dataSize += (sizeof(block::detail::KeyLengthType) + key.length());
  }

  std::string buf;
  buf.reserve(dataSize);

  // State hash.
  buf.append(reinterpret_cast<const char *>(data.stateHash.data()), data.stateHash.size());

  // Sorted deleted keys in [length, key] encoding.
  for (const auto &key : data.deletedKeys) {
    buf += concordUtils::toBigEndianStringBuffer(static_cast<block::detail::KeyLengthType>(key.length()));
    buf.append(key.data(), key.length());
  }

  return buf;
}

inline std::string serializeImp(const DatabaseLeafValue &val) {
  auto buf = serializeImp(val.addedInBlockId);
  if (val.deletedInBlockId.has_value()) {
    buf += serializeImp(*val.deletedInBlockId);
  } else {
    buf += serializeImp(detail::INVALID_BLOCK_ID);
  }
  return buf + val.leafNode.value.toString();
}

inline std::string serialize() { return std::string{}; }

template <typename T1, typename... T>
std::string serialize(const T1 &v1, const T &... v) {
  return serializeImp(v1) + serialize(v...);
}

template <typename T>
T deserialize(const concordUtils::Sliver &buf);

template <>
inline sparse_merkle::Hash deserialize<sparse_merkle::Hash>(const concordUtils::Sliver &buf) {
  Assert(buf.length() >= sparse_merkle::Hash::SIZE_IN_BYTES);
  return sparse_merkle::Hash{reinterpret_cast<const uint8_t *>(buf.data())};
}

template <>
inline sparse_merkle::Version deserialize<sparse_merkle::Version>(const concordUtils::Sliver &buf) {
  Assert(buf.length() >= sparse_merkle::Version::SIZE_IN_BYTES);
  return concordUtils::fromBigEndianBuffer<sparse_merkle::Version::Type>(buf.data());
}

template <>
inline std::vector<std::uint8_t> deserialize<std::vector<std::uint8_t>>(const concordUtils::Sliver &buf) {
  std::vector<std::uint8_t> vec;
  for (auto i = 0u; i < buf.length(); ++i) {
    vec.push_back(buf[i]);
  }
  return vec;
}

template <>
inline sparse_merkle::NibblePath deserialize<sparse_merkle::NibblePath>(const concordUtils::Sliver &buf) {
  Assert(buf.length() >= sizeof(std::uint8_t));
  const auto nibbleCount = static_cast<std::size_t>(buf[0]);
  const auto path = deserialize<std::vector<std::uint8_t>>(
      concordUtils::Sliver{buf, sizeof(std::uint8_t), buf.length() - sizeof(std::uint8_t)});
  return sparse_merkle::NibblePath{nibbleCount, path};
}

template <>
inline sparse_merkle::LeafKey deserialize<sparse_merkle::LeafKey>(const concordUtils::Sliver &buf) {
  Assert(buf.length() >= sparse_merkle::LeafKey::SIZE_IN_BYTES);
  return sparse_merkle::LeafKey{deserialize<sparse_merkle::Hash>(buf),
                                deserialize<sparse_merkle::Version>(concordUtils::Sliver{
                                    buf, sparse_merkle::Hash::SIZE_IN_BYTES, sparse_merkle::Version::SIZE_IN_BYTES})};
}

template <>
inline sparse_merkle::InternalNodeKey deserialize<sparse_merkle::InternalNodeKey>(const concordUtils::Sliver &buf) {
  return sparse_merkle::InternalNodeKey{
      deserialize<sparse_merkle::Version>(buf),
      deserialize<sparse_merkle::NibblePath>(concordUtils::Sliver{
          buf, sparse_merkle::Version::SIZE_IN_BYTES, buf.length() - sparse_merkle::Version::SIZE_IN_BYTES})};
}

template <>
inline sparse_merkle::LeafChild deserialize<sparse_merkle::LeafChild>(const concordUtils::Sliver &buf) {
  Assert(buf.length() >= sparse_merkle::LeafChild::SIZE_IN_BYTES);
  return sparse_merkle::LeafChild{deserialize<sparse_merkle::Hash>(buf),
                                  deserialize<sparse_merkle::LeafKey>(concordUtils::Sliver{
                                      buf, sparse_merkle::Hash::SIZE_IN_BYTES, sparse_merkle::LeafKey::SIZE_IN_BYTES})};
}

template <>
inline sparse_merkle::InternalChild deserialize<sparse_merkle::InternalChild>(const concordUtils::Sliver &buf) {
  Assert(buf.length() >= sparse_merkle::InternalChild::SIZE_IN_BYTES);
  return sparse_merkle::InternalChild{
      deserialize<sparse_merkle::Hash>(buf),
      deserialize<sparse_merkle::Version>(
          concordUtils::Sliver{buf, sparse_merkle::Hash::SIZE_IN_BYTES, sparse_merkle::Version::SIZE_IN_BYTES})};
}

template <>
inline sparse_merkle::BatchedInternalNode deserialize<sparse_merkle::BatchedInternalNode>(
    const concordUtils::Sliver &buf) {
  if (buf.empty()) {
    return sparse_merkle::BatchedInternalNode{};
  }

  Assert(buf.length() >= sizeof(BatchedInternalMaskType));
  sparse_merkle::BatchedInternalNode::Children children;
  const auto mask = concordUtils::fromBigEndianBuffer<BatchedInternalMaskType>(buf.data());
  auto childrenBuf =
      concordUtils::Sliver{buf, sizeof(BatchedInternalMaskType), buf.length() - sizeof(BatchedInternalMaskType)};
  for (auto i = 0u; i < sparse_merkle::BatchedInternalNode::MAX_CHILDREN; ++i) {
    if (mask & (1 << i)) {
      // First byte is the type as per BatchedInternalNodeChildType .
      const auto type = getInternalChildType(childrenBuf);
      childrenBuf = concordUtils::Sliver{childrenBuf, 1, childrenBuf.length() - 1};
      switch (type) {
        case BatchedInternalNodeChildType::Leaf:
          children[i] = deserialize<sparse_merkle::LeafChild>(childrenBuf);
          childrenBuf = concordUtils::Sliver{childrenBuf,
                                             sparse_merkle::LeafChild::SIZE_IN_BYTES,
                                             childrenBuf.length() - sparse_merkle::LeafChild::SIZE_IN_BYTES};
          break;
        case BatchedInternalNodeChildType::Internal:
          children[i] = deserialize<sparse_merkle::InternalChild>(childrenBuf);
          childrenBuf = concordUtils::Sliver{childrenBuf,
                                             sparse_merkle::InternalChild::SIZE_IN_BYTES,
                                             childrenBuf.length() - sparse_merkle::InternalChild::SIZE_IN_BYTES};
          break;
      }
    }
  }
  return children;
}

template <>
inline block::detail::Node deserialize<block::detail::Node>(const concordUtils::Sliver &buf) {
  Assert(buf.length() >= block::detail::Node::MIN_SIZE);

  auto offset = std::size_t{0};
  auto node = block::detail::Node{};

  // Block ID.
  node.blockId = concordUtils::fromBigEndianBuffer<block::detail::Node::BlockIdType>(buf.data() + offset);
  offset += sizeof(block::detail::Node::BlockIdType);

  // Parent digest.
  node.setParentDigest(buf.data() + offset);
  offset += block::detail::Node::PARENT_DIGEST_SIZE;

  // State hash.
  node.stateHash = sparse_merkle::Hash{reinterpret_cast<const std::uint8_t *>(buf.data()) + offset};
  offset += block::detail::Node::STATE_HASH_SIZE;

  // State root version.
  node.stateRootVersion = concordUtils::fromBigEndianBuffer<sparse_merkle::Version::Type>(buf.data() + offset);
  offset += block::detail::Node::STATE_ROOT_VERSION_SIZE;

  // Keys follow the static length of Node::MIN_SIZE bytes.
  auto keyBuffer =
      concordUtils::Sliver{buf, block::detail::Node::MIN_SIZE, buf.length() - block::detail::Node::MIN_SIZE};
  while (!keyBuffer.empty()) {
    Assert(keyBuffer.length() >= block::detail::Node::MIN_KEY_SIZE);

    // Key data.
    block::detail::KeyData keyData;
    if (keyBuffer[0]) {
      keyData.deleted = true;
    }

    // Key length.
    const auto keyLen = concordUtils::fromBigEndianBuffer<block::detail::KeyLengthType>(keyBuffer.data() + 1);
    Assert(keyLen <= (keyBuffer.length() - block::detail::Node::MIN_KEY_SIZE));
    node.keys.emplace(concordUtils::Sliver{keyBuffer, block::detail::Node::MIN_KEY_SIZE, keyLen}, keyData);

    keyBuffer = concordUtils::Sliver{keyBuffer,
                                     block::detail::Node::MIN_KEY_SIZE + keyLen,
                                     keyBuffer.length() - (block::detail::Node::MIN_KEY_SIZE + keyLen)};
  }

  return node;
}

template <>
inline block::detail::RawBlockMerkleData deserialize<block::detail::RawBlockMerkleData>(
    const concordUtils::Sliver &buf) {
  Assert(buf.length() >= block::detail::RawBlockMerkleData::MIN_SIZE);

  auto offset = std::size_t{0};
  auto data = block::detail::RawBlockMerkleData{};

  // State hash.
  data.stateHash = sparse_merkle::Hash{reinterpret_cast<const std::uint8_t *>(buf.data()) + offset};
  offset += block::detail::RawBlockMerkleData::STATE_HASH_SIZE;

  // Deleted keys follow.
  auto keyBuffer = concordUtils::Sliver{
      buf, block::detail::RawBlockMerkleData::MIN_SIZE, buf.length() - block::detail::RawBlockMerkleData::MIN_SIZE};
  while (!keyBuffer.empty()) {
    Assert(keyBuffer.length() >= block::detail::RawBlockMerkleData::MIN_KEY_SIZE);

    // Key length.
    const auto keyLen = concordUtils::fromBigEndianBuffer<block::detail::KeyLengthType>(keyBuffer.data());
    Assert(keyLen <= (keyBuffer.length() - block::detail::RawBlockMerkleData::MIN_KEY_SIZE));
    data.deletedKeys.insert(concordUtils::Sliver{keyBuffer, block::detail::RawBlockMerkleData::MIN_KEY_SIZE, keyLen});

    keyBuffer = concordUtils::Sliver{keyBuffer,
                                     block::detail::RawBlockMerkleData::MIN_KEY_SIZE + keyLen,
                                     keyBuffer.length() - (block::detail::RawBlockMerkleData::MIN_KEY_SIZE + keyLen)};
  }

  return data;
}

// Deserializes the state root version from a serialized block::detail::Node .
inline sparse_merkle::Version deserializeStateRootVersion(const concordUtils::Sliver &buf) {
  Assert(buf.length() >= block::detail::Node::MIN_SIZE);
  constexpr auto offset = sizeof(block::detail::Node::BlockIdType) + block::detail::Node::PARENT_DIGEST_SIZE +
                          block::detail::Node::STATE_HASH_SIZE;
  return concordUtils::fromBigEndianBuffer<sparse_merkle::Version::Type>(buf.data() + offset);
}

template <>
inline DatabaseLeafValue deserialize<DatabaseLeafValue>(const concordUtils::Sliver &buf) {
  Assert(buf.length() >= DatabaseLeafValue::MIN_SIZE);
  constexpr auto blockIdSize = sizeof(DatabaseLeafValue::BlockIdType);
  const auto addedInBlock = concordUtils::fromBigEndianBuffer<DatabaseLeafValue::BlockIdType>(buf.data());
  const auto deletedInBlock =
      concordUtils::fromBigEndianBuffer<DatabaseLeafValue::BlockIdType>(buf.data() + blockIdSize);
  const auto leafNode = sparse_merkle::LeafNode{
      concordUtils::Sliver{buf, DatabaseLeafValue::MIN_SIZE, buf.length() - DatabaseLeafValue::MIN_SIZE}};
  if (deletedInBlock != detail::INVALID_BLOCK_ID) {
    return DatabaseLeafValue{addedInBlock, leafNode, deletedInBlock};
  }
  return DatabaseLeafValue{addedInBlock, leafNode};
}

}  // namespace detail

}  // namespace concord::kvbc::v2MerkleTree
