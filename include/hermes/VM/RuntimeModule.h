/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the LICENSE
 * file in the root directory of this source tree.
 */
#ifndef HERMES_VM_RUNTIMEMODULE_H
#define HERMES_VM_RUNTIMEMODULE_H

#include "hermes/BCGen/HBC/BytecodeDataProvider.h"
#include "hermes/Support/HashString.h"
#include "hermes/VM/CodeBlock.h"
#include "hermes/VM/IdentifierTable.h"
#include "hermes/VM/StringRefUtils.h"
#include "hermes/VM/WeakRef.h"

#include "llvm/ADT/simple_ilist.h"

namespace hermes {
namespace vm {

class CodeBlock;
class Runtime;

using StringID = uint32_t;

namespace detail {
/// Unit tests need to call into this function. We cannot expose the
/// templated version as its definition is in the cpp file, and will
/// cause a link error.
StringID mapString(RuntimeModule &module, const char *str);
} // namespace detail

/// Flags supporting RuntimeModule.
union RuntimeModuleFlags {
  struct {
    /// Whether this runtime module should persist in memory (i.e. never get
    /// freed even when refCount_ goes to 0.) This is needed when we want to
    /// have lazy identifiers whose string content is a pointer to the string
    /// storage in the bytecode module. We should only make the first (biggest)
    /// module persistent.
    bool persistent : 1;

    /// Whether this runtime module's epilogue should be hidden in
    /// runtime.getEpilogues().
    bool hidesEpilogue : 1;
  };
  uint8_t flags;
  RuntimeModuleFlags() : flags(0) {}
};

/// This class is used to store the non-instruction information needed to
/// execute code. The RuntimeModule owns a BytecodeModule, from which it copies
/// the string ID map and function map. Every CodeBlock contains a reference to
/// the RuntimeModule that contains its relevant information. Whenever a
/// JSFunction is created/destroyed, it will update the reference count of the
/// runtime module following through the code block.
/// CodeBlock's bytecode buffers live in a BytecodeFunction, which is owned by
/// BytecodeModule, which is stored in this RuntimeModule.
///
/// If executing a CodeBlock, construct a RuntimeModule with
/// RuntimeModule::create(runtime) first. If the string ID map and function map
/// are needed, then use RuntimeModule::create(runtime, bytecodeModule).
///
/// All RuntimeModule-s associated with a \c Runtime are kept together in a
/// linked list which can be walked to perform memory management tasks.
class RuntimeModule final : public llvm::ilist_node<RuntimeModule> {
 private:
  friend StringID detail::mapString(RuntimeModule &module, const char *str);

  /// The runtime this module is associated with.
  Runtime *runtime_;

  /// The table maps from a sequential string id in the bytecode to an
  /// SymbolID.
  std::vector<SymbolID> stringIDMap_;

  /// Weak pointer to a GC-managed Domain that owns this RuntimeModule.
  /// NOTE: This will not be made invalid through marking, because the domain
  /// updates the WeakRefs on the RuntimeModule when it is marked.
  /// We use WeakRef<Domain> here to express that the RuntimeModule does not own
  /// the Domain.
  /// We avoid using a raw pointer to Domain because we must be able to update
  /// it when the GC moves the Domain.
  WeakRef<Domain> domain_;

  /// The table maps from a function index to a CodeBlock.
  std::vector<CodeBlock *> functionMap_{};

  /// The byte-code provider for this RuntimeModule. The RuntimeModule is
  /// designed to own the provider exclusively, especially because in some
  /// cases the bytecode can be modified (e.g. for breakpoints). This however
  /// is a shared_ptr<> instead of unique_ptr<> for a pragmatic reason - when
  /// we run performance tests, we want to re-use a BCProvider between runtimes
  /// in order to minimize the noise.
  std::shared_ptr<hbc::BCProvider> bcProvider_{};

  /// Flags associated with the module.
  RuntimeModuleFlags flags_{};

  /// The sourceURL set explicitly for the module, or empty if none.
  std::string sourceURL_{};

  /// A list of RuntimeModules that this module depends on, specifically
  /// because they're lazily compiled and should be considered a unit.
  std::vector<RuntimeModule *> dependentModules_{};

  /// A map from NewObjectWithBuffer's <keyBufferIndex, numLiterals> tuple to
  /// its shared hidden class.
  /// During hashing, keyBufferIndex takes the top 24bits while numLiterals
  /// becomes the lower 8bits of the key.
  /// Cacheing will be skipped if keyBufferIndex is >= 2^24.
  llvm::DenseMap<uint32_t, HiddenClass *> objectLiteralHiddenClasses_;

  /// A map from template object ids to template objects.
  llvm::DenseMap<uint32_t, JSObject *> templateMap_;

  /// Registers the created RuntimeModule with \param domain, resulting in
  /// \param domain owning it. The RuntimeModule will be freed when the
  /// domain is collected..
  explicit RuntimeModule(
      Runtime *runtime,
      Handle<Domain> domain,
      RuntimeModuleFlags flags,
      llvm::StringRef sourceURL);

  CodeBlock *getCodeBlockSlowPath(unsigned index);

 public:
  ~RuntimeModule();

  /// Creates a new RuntimeModule under \p runtime and imports the CJS
  /// module table into \p domain.
  /// \param runtime the runtime to use for the identifier table.
  /// \param bytecode the bytecode to import strings and functions from.
  /// \param sourceURL the filename to report in exception backtraces.
  /// \return a raw pointer to the runtime module.
  static CallResult<RuntimeModule *> create(
      Runtime *runtime,
      Handle<Domain> domain,
      std::shared_ptr<hbc::BCProvider> &&bytecode = nullptr,
      RuntimeModuleFlags flags = {},
      llvm::StringRef sourceURL = {});

  /// Creates a new RuntimeModule that is not yet initialized. It may be
  /// initialized later through lazy compilation.
  /// \param runtime the runtime to use for the identifier table.
  /// \return a raw pointer to the runtime module.
  static RuntimeModule *createUninitialized(
      Runtime *runtime,
      Handle<Domain> domain,
      RuntimeModuleFlags flags = {}) {
    return new RuntimeModule(runtime, domain, flags, "");
  }

#ifndef HERMESVM_LEAN
  /// Crates a lazy RuntimeModule as part of lazy compilation. This module
  /// will contain only one CodeBlock that points to \p function. This newly
  /// created RuntimeModule is going to be a dependent of the \p parent.
  static RuntimeModule *createLazyModule(
      Runtime *runtime,
      Handle<Domain> domain,
      RuntimeModule *parent,
      uint32_t functionID);

  /// If a CodeBlock in this module is compiled lazily, it generates a new
  /// RuntimeModule. The parent module should have a dependency on the child.
  void addDependency(RuntimeModule *module);

  /// Verifies that there is only one CodeBlock in this module, and return it.
  /// This is used when a lazy code block is created which should be the only
  /// block in the module.
  CodeBlock *getOnlyLazyCodeBlock() const {
    assert(functionMap_.size() == 1 && functionMap_[0] && "Not a lazy module?");
    return functionMap_[0];
  }

  /// Get the name symbol ID associated with the getOnlyLazyCodeBlock().
  SymbolID getLazyName();

  /// Initialize lazy modules created with \p createUninitialized.
  /// Calls `initialize` and does a bit of extra work.
  /// \param bytecode the bytecode data to initialize it with.
  void initializeLazy(std::unique_ptr<hbc::BCProvider> bytecode);
#endif

  /// Initialize modules created with \p createUninitialized,
  /// but do not import the CJS module table, allowing us to always succeed.
  /// \param bytecode the bytecode data to initialize it with.
  void initializeWithoutCJSModules(std::shared_ptr<hbc::BCProvider> &&bytecode);

  /// Initialize modules created with \p createUninitialized and import the CJS
  /// module table from the provided bytecode file.
  /// \param bytecode the bytecode data to initialize it with.
  LLVM_NODISCARD ExecutionStatus
  initialize(std::shared_ptr<hbc::BCProvider> &&bytecode);

  /// Prepares this RuntimeModule for the systematic destruction of all modules.
  /// Normal destruction is reference counted, but when the Runtime shuts down,
  /// we ignore that count and delete all in an arbitrary order.
  void prepareForRuntimeShutdown();

  /// For opcodes that use a stringID as identifier explicitly, we know that
  /// the compiler would have marked the stringID as identifier, and hence
  /// we should have created the symbol during identifier table initialization.
  /// The symbol must already exist in the map. This is a fast path.
  SymbolID getSymbolIDMustExist(StringID stringID) {
    assert(
        stringIDMap_[stringID].isValid() &&
        "Symbol must exist for this string ID");
    return stringIDMap_[stringID];
  }

  /// \return the \c SymbolID for a string by string index. The symbol may not
  /// already exist for this given string ID. Hence we may need to create it
  /// on the fly.
  SymbolID getSymbolIDFromStringID(StringID stringID) {
    SymbolID id = stringIDMap_[stringID];
    if (LLVM_UNLIKELY(!id.isValid())) {
      // Materialize this lazily created symbol.
      auto entry = bcProvider_->getStringTableEntry(stringID);
      id = createSymbolFromStringID(stringID, entry, llvm::None);
    }
    assert(id.isValid() && "Failed to create symbol for stringID");
    return id;
  }

  /// Gets the SymbolID and looks it up in the runtime's identifier table.
  /// \return the StringPrimitive for a string by string index.
  StringPrimitive *getStringPrimFromStringID(StringID stringID);

  /// \return the RegExp bytecode for a given regexp ID.
  llvm::ArrayRef<uint8_t> getRegExpBytecodeFromRegExpID(
      uint32_t regExpId) const;

  /// \return the number of functions in the function map.
  uint32_t getNumCodeBlocks() const {
    return functionMap_.size();
  }

  /// \return the CodeBlock for a function by function index.
  inline CodeBlock *getCodeBlockMayAllocate(unsigned index) {
    if (LLVM_LIKELY(functionMap_[index])) {
      return functionMap_[index];
    }
    return getCodeBlockSlowPath(index);
  }

  /// \return whether this RuntimeModule has been initialized.
  bool isInitialized() const {
    return !bcProvider_->isLazy();
  }

  const hbc::BCProvider *getBytecode() const {
    return bcProvider_.get();
  }

  hbc::BCProvider *getBytecode() {
    return bcProvider_.get();
  }

  std::shared_ptr<hbc::BCProvider> getBytecodeSharedPtr() {
    return bcProvider_;
  }

  /// \return true if the RuntimeModule has CJS modules that have not been
  /// statically resolved.
  bool hasCJSModules() const {
    return !getBytecode()->getCJSModuleTable().empty();
  }

  /// \return true if the RuntimeModule has CJS modules that have been resolved
  /// statically.
  bool hasCJSModulesStatic() const {
    return !getBytecode()->getCJSModuleTableStatic().empty();
  }

  /// \return the domain which owns this RuntimeModule.
  inline Handle<Domain> getDomain(Runtime *);

  /// \return a raw pointer to the domain which owns this RuntimeModule.
  inline Domain *getDomainUnsafe();

  /// \return a constant reference to the function map.
  const std::vector<CodeBlock *> &getFunctionMap() {
    return functionMap_;
  }

  /// \return the sourceURL, or an empty string if none.
  llvm::StringRef getSourceURL() const {
    return sourceURL_;
  }

  /// \return whether this module hides its epilogue from
  /// Runtime.getEpilogues().
  bool hidesEpilogue() const {
    return flags_.hidesEpilogue;
  }

  /// \return any trailing data after the real bytecode.
  llvm::ArrayRef<uint8_t> getEpilogue() const {
    return bcProvider_->getEpilogue();
  }

  /// Mark the non-weak roots owned by this RuntimeModule.
  void markRoots(SlotAcceptor &acceptor, bool markLongLived);

  /// Mark the weak roots owned by this RuntimeModule.
  void markWeakRoots(SlotAcceptor &acceptor);

  /// Mark the weak reference to the Domain which owns this RuntimeModule.
  void markDomainRef(GC *gc);

  /// \return an estimate of the size of additional memory used by this
  /// RuntimeModule.
  size_t additionalMemorySize() const;

  /// Find the cached hidden class for an object literal, if one exists.
  /// \param keyBufferIndex value of NewObjectWithBuffer instruction.
  /// \param numLiterals number of literals used from key buffer of
  /// NewObjectWithBuffer instruction.
  /// \return the cached hidden class.
  llvm::Optional<Handle<HiddenClass>> findCachedLiteralHiddenClass(
      unsigned keyBufferIndex,
      unsigned numLiterals) const;

  /// Try to cache the sharable hidden class for object literal. Cache will
  /// be skipped if keyBufferIndex is >= 2^24.
  /// \param keyBufferIndex value of NewObjectWithBuffer instruction.
  /// \param clazz the hidden class to cache.
  void tryCacheLiteralHiddenClass(unsigned keyBufferIndex, HiddenClass *clazz);

  /// Given \p templateObjectID, retrieve the cached template object.
  /// if it doesn't exist, return a nullptr.
  JSObject *findCachedTemplateObject(uint32_t templateObjID) {
    return templateMap_.lookup(templateObjID);
  }

  /// Cache a template object in the template map using a template object ID as
  /// key.
  /// \p templateObjID is the template object ID, and it should not already
  /// exist in the map.
  /// \p templateObj is the template object that we are caching.
  void cacheTemplateObject(
      uint32_t templateObjID,
      Handle<JSObject> templateObj) {
    assert(
        templateMap_.count(templateObjID) == 0 &&
        "The template object already exists.");
    templateMap_[templateObjID] = templateObj.get();
  }

 private:
  /// Import the string table from the supplied module.
  void importStringIDMap();

  /// Initialize functionMap_, without actually creating the code blocks.
  /// They will be created lazily when needed.
  void initializeFunctionMap();

  /// Import the CommonJS module table.
  /// Set every module to uninitialized, except for the first module.
  LLVM_NODISCARD ExecutionStatus importCJSModuleTable();

  /// Map the supplied string to a given \p stringID, register it in the
  /// identifier table, and \return the symbol ID.
  /// Computes the hash of the string when it's not supplied.
  template <typename T>
  SymbolID mapString(llvm::ArrayRef<T> str, StringID stringID) {
    return mapString(str, stringID, hermes::hashString(str));
  }

  /// Map the supplied string to a given \p stringID, register it in the
  /// identifier table, and \return the symbol ID.
  template <typename T>
  SymbolID mapString(llvm::ArrayRef<T> str, StringID stringID, uint32_t hash);

  /// Map the string at id \p stringID in the bytecode to \p rawSymbolID -- the
  /// ID for a predefined string.  If the symbol ID does not correspond to a
  /// predefined string, an assertion will be triggered (if they are enabled).
  SymbolID mapPredefined(StringID stringID, uint32_t rawSymbolID);

  /// Create a symbol from a given \p stringID, which is an index to the
  /// string table, corresponding to the entry \p entry. If \p mhash is not
  /// None, use it as the hash; otherwise compute the hash from the string
  /// contents. \return the created symbol ID.
  SymbolID createSymbolFromStringID(
      StringID stringID,
      const StringTableEntry &entry,
      OptValue<uint32_t> mhash);

  /// \return a unqiue hash key for object literal hidden class cache.
  /// \param keyBufferIndex value of NewObjectWithBuffer instruction(must be
  /// less than 2^24).
  /// \param numLiterals number of literals used from key buffer of
  /// NewObjectWithBuffer instruction(must be less than 256).
  static uint32_t getLiteralHiddenClassCacheHashKey(
      unsigned keyBufferIndex,
      unsigned numLiterals) {
    assert(
        canGenerateLiteralHiddenClassCacheKey(keyBufferIndex, numLiterals) &&
        "<keyBufferIndex, numLiterals> tuple can't be used as cache key.");
    return ((uint32_t)keyBufferIndex << 8) | numLiterals;
  }

  /// \return whether tuple <keyBufferIndex, numLiterals> can generate a
  /// hidden class literal cache hash key or not.
  /// \param keyBufferIndex value of NewObjectWithBuffer instruction; it must
  /// be less than 2^24 to be used as a cache key.
  /// \param keyBufferIndex value of NewObjectWithBuffer instruction. it must
  /// be less than 256 to be used as a cache key.
  static bool canGenerateLiteralHiddenClassCacheKey(
      uint32_t keyBufferIndex,
      unsigned numLiterals) {
    return (keyBufferIndex & 0xFF000000) == 0 && numLiterals < 256;
  }
};

using RuntimeModuleList = llvm::simple_ilist<RuntimeModule>;

} // namespace vm
} // namespace hermes

#endif
