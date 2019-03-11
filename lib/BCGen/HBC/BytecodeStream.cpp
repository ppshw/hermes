/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the LICENSE
 * file in the root directory of this source tree.
 */
#include "hermes/BCGen/HBC/BytecodeStream.h"

using namespace hermes;
using namespace hbc;

// ============================ File ============================
void BytecodeSerializer::serialize(BytecodeModule &BM, const SHA1 &sourceHash) {
  bool cjsModulesResolved = !BM.getCJSModuleTableStatic().empty();
  int32_t cjsModuleCount = cjsModulesResolved
      ? -BM.getCJSModuleTableStatic().size()
      : BM.getCJSModuleTable().size();

  BytecodeFileHeader header{MAGIC,
                            BYTECODE_VERSION,
                            sourceHash,
                            fileLength_,
                            BM.getGlobalFunctionIndex(),
                            BM.getNumFunctions(),
                            BM.getStringTableSize(),
                            BM.getIdentifierCount(),
                            stringTableBytes_,
                            BM.getStringStorageSize(),
                            static_cast<uint32_t>(BM.getRegExpTable().size()),
                            static_cast<uint32_t>(BM.getRegExpStorage().size()),
                            BM.getArrayBufferSize(),
                            BM.getObjectKeyBufferSize(),
                            BM.getObjectValueBufferSize(),
                            BM.getCJSModuleOffset(),
                            cjsModuleCount,
                            debugInfoOffset_,
                            BM.getBytecodeOptions()};
  writeBinary(header);
  // Sizes of file and function headers are tuned for good cache line packing.
  // If you reorder the format, try to avoid headers crossing cache lines.
  serializeFunctionTable(BM);

  serializeStringTable(BM);

  serializeArrayBuffer(BM);

  serializeObjectBuffer(BM);

  serializeRegExps(BM);

  serializeCJSModuleTable(BM);

  serializeFunctionsBytecode(BM);

  for (auto &entry : BM.getFunctionTable()) {
    serializeFunctionInfo(*entry);
  }

  serializeDebugInfo(BM);

  if (isLayout_) {
    finishLayout(BM);
    serialize(BM, sourceHash);
  }
}

void BytecodeSerializer::finishLayout(BytecodeModule &BM) {
  fileLength_ = loc_;
  assert(fileLength_ > 0 && "Empty file after layout");
  isLayout_ = false;
  loc_ = 0;
}

// ========================== Function Table ==========================
void BytecodeSerializer::serializeFunctionTable(BytecodeModule &BM) {
  for (auto &entry : BM.getFunctionTable()) {
    if (options_.stripDebugInfoSection) {
      // Change flag on the actual BF, so it's seen by serializeFunctionInfo.
      entry->mutableFlags().hasDebugInfo = false;
    }
    FunctionHeader header = entry->getHeader();
    writeBinary(SmallFuncHeader(header));
  }
}

// ========================== String Table ==========================
void BytecodeSerializer::serializeStringTable(BytecodeModule &BM) {
  auto stringTableBegin = loc_;
  std::vector<OverflowStringTableEntry> overflow;
  for (auto &entry : BM.getStringTable()) {
    SmallStringTableEntry small(entry, overflow.size());
    writeBinary(small);
    if (small.isOverflowed()) {
      overflow.emplace_back(entry.getOffset(), entry.getLength());
    }
  }
  writeBinaryArray(ArrayRef<OverflowStringTableEntry>(overflow));
  stringTableBytes_ = loc_ - stringTableBegin;
  writeBinaryArray(BM.getIdentifierHashes());
  writeBinaryArray(BM.getStringStorage());
}

// ========================== RegExps ==========================
void BytecodeSerializer::serializeRegExps(BytecodeModule &BM) {
  llvm::ArrayRef<RegExpTableEntry> table = BM.getRegExpTable();
  llvm::ArrayRef<unsigned char> storage = BM.getRegExpStorage();
  pad(4);
  writeBinaryArray(table);
  writeBinaryArray(storage);
}

// ========================== DebugInfo ==========================
void BytecodeSerializer::serializeDebugInfo(BytecodeModule &BM) {
  pad(4);
  const DebugInfo &info = BM.getDebugInfo();
  debugInfoOffset_ = loc_;

  if (options_.stripDebugInfoSection) {
    const DebugInfoHeader empty = {0, 0, 0, 0, 0};
    writeBinary(empty);
    return;
  }

  const llvm::ArrayRef<StringTableEntry> filenameTable =
      info.getFilenameTable();
  const llvm::ArrayRef<char> filenameStorage = info.getFilenameStorage();
  const DebugInfo::DebugFileRegionList &files = info.viewFiles();
  const StreamVector<uint8_t> &data = info.viewData();
  uint32_t lexOffset = info.lexicalDataOffset();

  DebugInfoHeader header{(uint32_t)filenameTable.size(),
                         (uint32_t)filenameStorage.size(),
                         (uint32_t)files.size(),
                         lexOffset,
                         (uint32_t)data.size()};
  writeBinary(header);
  writeBinaryArray(filenameTable);
  writeBinaryArray(filenameStorage);
  for (auto &file : files) {
    writeBinary(file);
  }
  writeBinaryArray(data.getData());
}

// ===================== CommonJS Module Table ======================
void BytecodeSerializer::serializeCJSModuleTable(BytecodeModule &BM) {
  pad(4);

  for (const auto &it : BM.getCJSModuleTable()) {
    writeBinary(it.first);
    writeBinary(it.second);
  }

  writeBinaryArray(BM.getCJSModuleTableStatic());
}

// ==================== Exception Handler Table =====================
void BytecodeSerializer::serializeExceptionHandlerTable(BytecodeFunction &BF) {
  if (!BF.hasExceptionHandlers())
    return;

  pad(INFO_ALIGNMENT);
  ExceptionHandlerTableHeader header{BF.getExceptionHandlerCount()};
  writeBinary(header);

  writeBinaryArray(BF.getExceptionHandlers());
}

// ========================= Array Buffer ==========================
void BytecodeSerializer::serializeArrayBuffer(BytecodeModule &BM) {
  writeBinaryArray(BM.getArrayBuffer());
}

void BytecodeSerializer::serializeObjectBuffer(BytecodeModule &BM) {
  auto objectKeyValBufferPair = BM.getObjectBuffer();

  writeBinaryArray(objectKeyValBufferPair.first);
  writeBinaryArray(objectKeyValBufferPair.second);
}

void BytecodeSerializer::serializeDebugOffsets(BytecodeFunction &BF) {
  if (options_.stripDebugInfoSection || !BF.hasDebugInfo()) {
    return;
  }

  pad(INFO_ALIGNMENT);
  auto *offsets = BF.getDebugOffsets();
  writeBinary(*offsets);
}

// ============================ Function ============================
void BytecodeSerializer::serializeFunctionsBytecode(BytecodeModule &BM) {
  // Map from opcodes and jumptables to offsets, used to deduplicate bytecode.
  using DedupKey =
      std::pair<llvm::ArrayRef<opcode_atom_t>, llvm::ArrayRef<uint32_t>>;
  llvm::DenseMap<DedupKey, uint32_t> bcMap;
  for (auto &entry : BM.getFunctionTable()) {
    if (options_.optimizationEnabled) {
      // If identical bytecode exists, we'll reuse it.
      bool reuse = false;
      if (isLayout_) {
        // Deduplicate the bytecode during layout phase.
        DedupKey key =
            std::make_pair(entry->getOpcodeArray(), entry->getJumpTables());
        auto pair = bcMap.insert(std::make_pair(key, loc_));
        if (!pair.second) {
          reuse = true;
          entry->setOffset(pair.first->second);
        }
      } else {
        // Cheaply determine whether bytecode was deduplicated.
        assert(entry->getOffset() && "Function lacks offset after layout");
        assert(entry->getOffset() <= loc_ && "Function has too large offset");
        reuse = entry->getOffset() < loc_;
      }
      if (reuse) {
        continue;
      }
    }

    // Set the offset of this function's bytecode.
    if (isLayout_) {
      entry->setOffset(loc_);
    }

    // Serialize opcodes.
    writeBinaryArray(entry->getOpcodeArray());

    // Serialize the jump table after the opcode block.
    pad(sizeof(uint32_t));

    writeBinaryArray(entry->getJumpTables());

    if (options_.padFunctionBodiesPercent) {
      size_t size = entry->getOpcodeArray().size();
      size = (size * options_.padFunctionBodiesPercent) / 100;
      while (size--)
        writeBinary('\0');
      pad(sizeof(uint32_t));
    }
  }
}

void BytecodeSerializer::serializeFunctionInfo(BytecodeFunction &BF) {
  // Set the offset of this function's info. Any subsection that is present is
  // aligned to INFO_ALIGNMENT, so we also align the recorded offset to that.
  if (isLayout_) {
    BF.setInfoOffset(llvm::alignTo(loc_, INFO_ALIGNMENT));
  }

  // Write large header if it doesn't fit in a small.
  FunctionHeader header = BF.getHeader();
  if (SmallFuncHeader(header).flags.overflowed) {
    pad(INFO_ALIGNMENT);
    writeBinary(header);
  }

  // Serialize exception handlers.
  serializeExceptionHandlerTable(BF);

  // Add offset in debug info (if function has debug info).
  serializeDebugOffsets(BF);
}
