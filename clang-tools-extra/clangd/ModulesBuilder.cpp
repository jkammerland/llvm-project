//===----------------- ModulesBuilder.cpp ------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ModulesBuilder.h"
#include "CompileCommands.h"
#include "Compiler.h"
#include "support/Logger.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Serialization/ASTReader.h"
#include "clang/Serialization/ModuleCache.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/xxhash.h"

#include <optional>
#include <queue>
#include <tuple>

namespace clang {
namespace clangd {

namespace {

llvm::cl::opt<bool> DebugModulesBuilder(
    "debug-modules-builder",
    llvm::cl::desc("Don't remove clangd's built module files for debugging. "
                   "Remember to remove them later after debugging."),
    llvm::cl::init(false));

struct ModuleInputStamp {
  std::string Path;
  uint64_t StoredSize = 0;
  int64_t StoredTime = 0;
  uint64_t ContentHash = 0;
};

struct ModuleImportStamp {
  std::string ModuleName;
  std::string Path;
  uint64_t ContentHash = 0;
};

struct ModuleInputManifest {
  uint64_t ModuleFileHash = 0;
  std::vector<ModuleInputStamp> Inputs;
  std::vector<ModuleImportStamp> Imports;
};

llvm::SmallString<256> getModuleCacheRoot() {
  llvm::SmallString<256> Root;
  if (llvm::sys::path::cache_directory(Root))
    llvm::sys::path::append(Root, "clangd");
  else
    llvm::sys::path::system_temp_directory(/*erasedOnReboot=*/false, Root);
  llvm::sys::path::append(Root, "module_files");
  llvm::sys::fs::create_directories(Root);
  return Root;
}

llvm::SmallString<256>
getStableModuleFilesPath(PathRef ModuleUnitFileName,
                         llvm::StringRef StableModuleVariantFingerprint) {
  llvm::SmallString<256> Result = getModuleCacheRoot();
  llvm::SmallString<128> StablePrefix =
      llvm::sys::path::filename(ModuleUnitFileName);
  StablePrefix.push_back('-');
  StablePrefix.append(StableModuleVariantFingerprint);
  llvm::sys::path::append(Result, StablePrefix);
  return Result;
}

std::string getTemporaryModuleFilesPath(PathRef StableModuleFilesPath) {
  llvm::SmallString<256> ResultPattern(StableModuleFilesPath);
  ResultPattern.append(".tmp-%%%%%%");
  llvm::SmallString<256> Result;
  llvm::sys::fs::createUniquePath(ResultPattern, Result,
                                  /*MakeAbsolute=*/false);
  llvm::sys::fs::create_directories(Result);
  return Result.str().str();
}

// Get a unique module file path under \param ModuleFilesPrefix.
std::string getModuleFilePath(llvm::StringRef ModuleName,
                              PathRef ModuleFilesPrefix) {
  llvm::SmallString<256> ModuleFilePath(ModuleFilesPrefix);
  auto [PrimaryModuleName, PartitionName] = ModuleName.split(':');
  llvm::sys::path::append(ModuleFilePath, PrimaryModuleName);
  if (!PartitionName.empty()) {
    ModuleFilePath.append("-");
    ModuleFilePath.append(PartitionName);
  }

  ModuleFilePath.append(".pcm");
  return std::string(ModuleFilePath);
}

std::string getModuleContextHashFilePath(PathRef ModuleFilePath) {
  return (ModuleFilePath + ".ctxhash").str();
}

std::string getModuleInputManifestFilePath(PathRef ModuleFilePath) {
  return (ModuleFilePath + ".inputs.json").str();
}

void writeModuleContextHashFile(PathRef ModuleFilePath,
                                llvm::StringRef ContextHash) {
  std::error_code EC;
  llvm::raw_fd_ostream OS(getModuleContextHashFilePath(ModuleFilePath), EC);
  if (EC) {
    vlog("Failed to write module context hash file for {0}: {1}",
         ModuleFilePath, EC.message());
    return;
  }
  OS << ContextHash;
}

std::optional<std::string>
readModuleContextHashFile(PathRef ModuleFilePath,
                          llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS) {
  auto Buffer =
      VFS->getBufferForFile(getModuleContextHashFilePath(ModuleFilePath));
  if (!Buffer)
    return std::nullopt;
  return llvm::StringRef(Buffer.get()->getBuffer()).trim().str();
}

void writeModuleInputManifestFile(PathRef ModuleFilePath,
                                  const ModuleInputManifest &Manifest) {
  llvm::json::Array InputArray;
  for (const auto &Input : Manifest.Inputs) {
    llvm::json::Object Entry;
    Entry["path"] = Input.Path;
    Entry["size"] = static_cast<int64_t>(Input.StoredSize);
    Entry["mtime"] = Input.StoredTime;
    Entry["hash"] = llvm::utohexstr(Input.ContentHash);
    InputArray.push_back(std::move(Entry));
  }

  llvm::json::Array ImportArray;
  for (const auto &Import : Manifest.Imports) {
    llvm::json::Object Entry;
    Entry["module"] = Import.ModuleName;
    Entry["path"] = Import.Path;
    Entry["hash"] = llvm::utohexstr(Import.ContentHash);
    ImportArray.push_back(std::move(Entry));
  }

  llvm::json::Object Root;
  Root["module_hash"] = llvm::utohexstr(Manifest.ModuleFileHash);
  Root["inputs"] = std::move(InputArray);
  Root["imports"] = std::move(ImportArray);

  std::error_code EC;
  llvm::raw_fd_ostream OS(getModuleInputManifestFilePath(ModuleFilePath), EC);
  if (EC) {
    vlog("Failed to write module input manifest for {0}: {1}", ModuleFilePath,
         EC.message());
    return;
  }
  OS << llvm::formatv("{0:2}", llvm::json::Value(std::move(Root)));
}

std::optional<ModuleInputManifest>
readModuleInputManifestFile(PathRef ModuleFilePath,
                            llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS) {
  auto Buffer =
      VFS->getBufferForFile(getModuleInputManifestFilePath(ModuleFilePath));
  if (!Buffer)
    return std::nullopt;

  llvm::Expected<llvm::json::Value> Parsed =
      llvm::json::parse(Buffer.get()->getBuffer());
  if (!Parsed) {
    llvm::consumeError(Parsed.takeError());
    return std::nullopt;
  }

  const auto *Root = Parsed->getAsObject();
  if (!Root)
    return std::nullopt;
  std::optional<llvm::StringRef> ModuleHash = Root->getString("module_hash");
  const auto *InputArray = Root->getArray("inputs");
  if (!ModuleHash || !InputArray)
    return std::nullopt;
  const auto *ImportArray = Root->getArray("imports");

  ModuleInputManifest Manifest;
  if (ModuleHash->getAsInteger(16, Manifest.ModuleFileHash))
    return std::nullopt;

  Manifest.Inputs.reserve(InputArray->size());
  for (const auto &EntryValue : *InputArray) {
    const auto *Entry = EntryValue.getAsObject();
    if (!Entry)
      return std::nullopt;

    ModuleInputStamp Input;
    std::optional<llvm::StringRef> Path = Entry->getString("path");
    std::optional<int64_t> Size = Entry->getInteger("size");
    std::optional<int64_t> Time = Entry->getInteger("mtime");
    std::optional<llvm::StringRef> Hash = Entry->getString("hash");
    if (!Path || !Size || !Time || !Hash)
      return std::nullopt;

    uint64_t ParsedHash = 0;
    if (Hash->getAsInteger(16, ParsedHash))
      return std::nullopt;

    Input.Path = Path->str();
    Input.StoredSize = static_cast<uint64_t>(*Size);
    Input.StoredTime = *Time;
    Input.ContentHash = ParsedHash;
    Manifest.Inputs.push_back(std::move(Input));
  }

  if (ImportArray) {
    Manifest.Imports.reserve(ImportArray->size());
    for (const auto &EntryValue : *ImportArray) {
      const auto *Entry = EntryValue.getAsObject();
      if (!Entry)
        return std::nullopt;

      ModuleImportStamp Import;
      std::optional<llvm::StringRef> Module = Entry->getString("module");
      std::optional<llvm::StringRef> Path = Entry->getString("path");
      std::optional<llvm::StringRef> Hash = Entry->getString("hash");
      if (!Module || !Path || !Hash)
        return std::nullopt;

      uint64_t ParsedHash = 0;
      if (Hash->getAsInteger(16, ParsedHash))
        return std::nullopt;

      Import.ModuleName = Module->str();
      Import.Path = Path->str();
      Import.ContentHash = ParsedHash;
      Manifest.Imports.push_back(std::move(Import));
    }
  }
  return Manifest;
}

void removeModuleArtifacts(PathRef ModuleFilePath) {
  llvm::sys::fs::remove(ModuleFilePath);
  llvm::sys::fs::remove(getModuleContextHashFilePath(ModuleFilePath));
  llvm::sys::fs::remove(getModuleInputManifestFilePath(ModuleFilePath));
  llvm::sys::fs::remove(llvm::sys::path::parent_path(ModuleFilePath));
}

std::optional<uint64_t>
getFileContentHash(PathRef FilePath,
                   llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS) {
  auto Buffer = VFS->getBufferForFile(FilePath);
  if (!Buffer)
    return std::nullopt;
  return static_cast<uint64_t>(llvm::xxh3_64bits(Buffer.get()->getBuffer()));
}

std::string getResolvedModuleSourceIdentity(
    PathRef ModuleUnitFileName,
    llvm::StringRef WorkingDirectory,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS) {
  // ProjectModules may return relative paths, so resolve them the same way the
  // module compile command would before canonicalizing.
  llvm::SmallString<256> AbsolutePath(ModuleUnitFileName);
  if (!WorkingDirectory.empty() &&
      !llvm::sys::path::is_absolute(AbsolutePath)) {
    llvm::SmallString<256> Tmp(WorkingDirectory);
    llvm::sys::path::append(Tmp, AbsolutePath);
    AbsolutePath = std::move(Tmp);
  }
  llvm::SmallString<256> ResolvedPath;
  if (!VFS || VFS->getRealPath(AbsolutePath, ResolvedPath))
    return AbsolutePath.str().str();
  return ResolvedPath.str().str();
}

std::string
getCompileCommandFingerprint(const tooling::CompileCommand &CompileCommand) {
  std::vector<std::string> CommandLine = CompileCommand.CommandLine;
  static const auto *StripOutputArgs = [] {
    auto *Stripper = new ArgStripper();
    // clangd rewrites module output locations, so output-path-only changes
    // should not invalidate reusable BMI fingerprints.
    Stripper->strip("-o");
    Stripper->strip("/Fo");
    return Stripper;
  }();
  StripOutputArgs->process(CommandLine);

  llvm::SmallString<512> FingerprintInput;
  auto AppendField = [&](llvm::StringRef Field) {
    FingerprintInput.append(Field);
    FingerprintInput.push_back('\0');
  };
  AppendField(CompileCommand.Directory);
  AppendField(CompileCommand.Filename);
  for (const auto &Arg : CommandLine)
    AppendField(Arg);
  return llvm::utohexstr(llvm::xxh3_64bits(llvm::toStringRef(FingerprintInput)));
}

void appendStableKeyField(llvm::raw_ostream &OS, llvm::StringRef Field) {
  OS << Field;
  OS << '\0';
}

class SingleViewThreadsafeFS : public ThreadsafeFS {
public:
  explicit SingleViewThreadsafeFS(
      llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FS)
      : FS(std::move(FS)) {}

private:
  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> viewImpl() const override {
    return FS;
  }

  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FS;
};

std::unique_ptr<CompilerInvocation>
buildCompilerInvocationForCommand(const tooling::CompileCommand &Command,
                                  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem>
                                      VFS) {
  SingleViewThreadsafeFS TFS(std::move(VFS));
  ParseInputs Inputs;
  Inputs.TFS = &TFS;
  Inputs.CompileCommand = Command;
  IgnoreDiagnostics IgnoreDiags;
  return buildCompilerInvocation(Inputs, IgnoreDiags);
}

struct ModuleLookupConfiguration {
  std::unique_ptr<CompilerInvocation> CI;
  std::string SourceIdentity;
  std::string CommandFingerprint;
};

ModuleLookupConfiguration
getModuleLookupConfiguration(PathRef ModuleUnitFileName,
                             const GlobalCompilationDatabase &CDB,
                             const ThreadsafeFS &TFS) {
  ModuleLookupConfiguration Config;
  if (auto Cmd = CDB.getCompileCommand(ModuleUnitFileName)) {
    Config.SourceIdentity = getResolvedModuleSourceIdentity(
        ModuleUnitFileName, Cmd->Directory, TFS.view(std::nullopt));
    Config.CommandFingerprint = getCompileCommandFingerprint(*Cmd);
    ParseInputs Inputs;
    Inputs.TFS = &TFS;
    Inputs.CompileCommand = std::move(*Cmd);
    IgnoreDiagnostics IgnoreDiags;
    Config.CI = buildCompilerInvocation(Inputs, IgnoreDiags);
  } else {
    Config.SourceIdentity =
        getResolvedModuleSourceIdentity(ModuleUnitFileName, "", TFS.view(std::nullopt));
  }
  return Config;
}

void applyModuleBuildInvocationSettings(CompilerInvocation &CI) {
  // In clang's driver, we suppress ODR checks in GMF while building modules.
  // Keep reuse validation aligned with that mode.
  CI.getLangOpts().SkipODRCheckInGMF = true;

  // Hash the contents of input files and preserve comments so reused BMIs match
  // the way clangd originally built them.
  CI.getHeaderSearchOpts().ModulesValidateSystemHeaders = true;
  CI.getHeaderSearchOpts().ValidateASTInputFilesContent = true;
  CI.getPreprocessorOpts().WriteCommentListToPCH = true;
  CI.getPreprocessorOpts().WriteCommentListToNamedModules = true;
}

bool hasCompatibleImporterConfiguration(const CompilerInvocation &ModuleBuildCI,
                                        const CompilerInvocation &ImporterCI) {
  const auto &ModuleTarget = ModuleBuildCI.getTargetOpts();
  const auto &ImporterTarget = ImporterCI.getTargetOpts();
  if (std::tie(ModuleTarget.Triple, ModuleTarget.HostTriple, ModuleTarget.CPU,
               ModuleTarget.TuneCPU, ModuleTarget.FPMath, ModuleTarget.ABI,
               ModuleTarget.EABIVersion, ModuleTarget.LinkerVersion,
               ModuleTarget.FeaturesAsWritten, ModuleTarget.Features,
               ModuleTarget.ForceEnableInt128,
               ModuleTarget.NVPTXUseShortPointers,
               ModuleTarget.CodeObjectVersion,
               ModuleTarget.AMDGPUPrintfKindVal, ModuleTarget.CodeModel,
               ModuleTarget.LargeDataThreshold, ModuleTarget.SDKVersion,
               ModuleTarget.DarwinTargetVariantTriple,
               ModuleTarget.DarwinTargetVariantSDKVersion,
               ModuleTarget.DxilValidatorVersion, ModuleTarget.HLSLEntry) !=
      std::tie(ImporterTarget.Triple, ImporterTarget.HostTriple,
               ImporterTarget.CPU, ImporterTarget.TuneCPU,
               ImporterTarget.FPMath, ImporterTarget.ABI,
               ImporterTarget.EABIVersion, ImporterTarget.LinkerVersion,
               ImporterTarget.FeaturesAsWritten, ImporterTarget.Features,
               ImporterTarget.ForceEnableInt128,
               ImporterTarget.NVPTXUseShortPointers,
               ImporterTarget.CodeObjectVersion,
               ImporterTarget.AMDGPUPrintfKindVal, ImporterTarget.CodeModel,
               ImporterTarget.LargeDataThreshold, ImporterTarget.SDKVersion,
               ImporterTarget.DarwinTargetVariantTriple,
               ImporterTarget.DarwinTargetVariantSDKVersion,
               ImporterTarget.DxilValidatorVersion,
               ImporterTarget.HLSLEntry))
    return false;

  return ModuleBuildCI.getFrontendOpts().AuxTriple ==
         ImporterCI.getFrontendOpts().AuxTriple;
}

bool IsModuleFileUpToDate(PathRef ModuleFilePath,
                          const PrerequisiteModules &RequisiteModules,
                          llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS,
                          const CompilerInvocation *CI,
                          bool CheckStoredContextHash = true,
                          bool AllowFastManifestValidation = true);

// FailedPrerequisiteModules - stands for the PrerequisiteModules which has
// errors happened during the building process.
class FailedPrerequisiteModules : public PrerequisiteModules {
public:
  ~FailedPrerequisiteModules() override = default;

  // We shouldn't adjust the compilation commands based on
  // FailedPrerequisiteModules.
  void adjustHeaderSearchOptions(HeaderSearchOptions &Options) const override {}

  // FailedPrerequisiteModules can never be reused.
  bool
  canReuse(const CompilerInvocation &CI,
           llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem>) const override {
    return false;
  }
};

/// Represents a reference to a module file (*.pcm).
class ModuleFile {
protected:
  ModuleFile(StringRef ModuleName, PathRef ModuleFilePath,
             StringRef ModuleSourceIdentity,
             StringRef CompileCommandFingerprint,
             StringRef RequiredSourceForLookup)
      : ModuleName(ModuleName.str()), ModuleFilePath(ModuleFilePath.str()),
        ModuleSourceIdentity(ModuleSourceIdentity.str()),
        CompileCommandFingerprint(CompileCommandFingerprint.str()),
        RequiredSourceForLookup(RequiredSourceForLookup.str()) {}

public:
  ModuleFile() = delete;

  ModuleFile(const ModuleFile &) = delete;
  ModuleFile operator=(const ModuleFile &) = delete;

  // The move constructor is needed for llvm::SmallVector.
  ModuleFile(ModuleFile &&Other)
      : ModuleName(std::move(Other.ModuleName)),
        ModuleFilePath(std::move(Other.ModuleFilePath)),
        ModuleSourceIdentity(std::move(Other.ModuleSourceIdentity)),
        CompileCommandFingerprint(std::move(Other.CompileCommandFingerprint)),
        RequiredSourceForLookup(std::move(Other.RequiredSourceForLookup)) {
    Other.ModuleName.clear();
    Other.ModuleFilePath.clear();
    Other.ModuleSourceIdentity.clear();
    Other.CompileCommandFingerprint.clear();
    Other.RequiredSourceForLookup.clear();
  }

  ModuleFile &operator=(ModuleFile &&Other) {
    if (this == &Other)
      return *this;

    this->~ModuleFile();
    new (this) ModuleFile(std::move(Other));
    return *this;
  }
  virtual ~ModuleFile() = default;

  StringRef getModuleName() const { return ModuleName; }

  StringRef getModuleFilePath() const { return ModuleFilePath; }
  StringRef getModuleSourceIdentity() const { return ModuleSourceIdentity; }
  StringRef getCompileCommandFingerprint() const {
    return CompileCommandFingerprint;
  }
  StringRef getRequiredSourceForLookup() const { return RequiredSourceForLookup; }

protected:
  std::string ModuleName;
  std::string ModuleFilePath;
  std::string ModuleSourceIdentity;
  std::string CompileCommandFingerprint;
  std::string RequiredSourceForLookup;
};

/// Represents a prebuilt module file which is not owned by us.
class PrebuiltModuleFile : public ModuleFile {
private:
  // private class to make sure the class can only be constructed by member
  // functions.
  struct CtorTag {};

public:
  PrebuiltModuleFile(StringRef ModuleName, PathRef ModuleFilePath,
                     StringRef ModuleSourceIdentity,
                     StringRef CompileCommandFingerprint,
                     StringRef RequiredSourceForLookup, CtorTag)
      : ModuleFile(ModuleName, ModuleFilePath, ModuleSourceIdentity,
                   CompileCommandFingerprint, RequiredSourceForLookup) {}

  static std::shared_ptr<PrebuiltModuleFile>
  make(StringRef ModuleName, PathRef ModuleFilePath,
       StringRef ModuleSourceIdentity, StringRef CompileCommandHash,
       StringRef RequiredSourceForLookup) {
    return std::make_shared<PrebuiltModuleFile>(ModuleName, ModuleFilePath,
                                                ModuleSourceIdentity,
                                                CompileCommandHash,
                                                RequiredSourceForLookup,
                                                CtorTag{});
  }
};

/// Represents a module file built by us. We're responsible to remove it.
class BuiltModuleFile : public ModuleFile {
private:
  // private class to make sure the class can only be constructed by member
  // functions.
  struct CtorTag {};

public:
  BuiltModuleFile(StringRef ModuleName, PathRef ModuleFilePath,
                  StringRef ModuleSourceIdentity,
                  StringRef CompileCommandFingerprint,
                  StringRef RequiredSourceForLookup,
                  bool RemoveOnDestruction, CtorTag)
      : ModuleFile(ModuleName, ModuleFilePath, ModuleSourceIdentity,
                   CompileCommandFingerprint, RequiredSourceForLookup),
        RemoveOnDestruction(RemoveOnDestruction) {}

  static std::shared_ptr<BuiltModuleFile> make(StringRef ModuleName,
                                               PathRef ModuleFilePath,
                                               StringRef ModuleSourceIdentity,
                                               StringRef CompileCommandHash,
                                               StringRef RequiredSourceForLookup,
                                               bool RemoveOnDestruction = true) {
    return std::make_shared<BuiltModuleFile>(ModuleName, ModuleFilePath,
                                             ModuleSourceIdentity,
                                             CompileCommandHash,
                                             RequiredSourceForLookup,
                                             RemoveOnDestruction,
                                             CtorTag{});
  }

  virtual ~BuiltModuleFile() {
    if (!ModuleFilePath.empty() && RemoveOnDestruction && !DebugModulesBuilder)
      removeModuleArtifacts(ModuleFilePath);
  }

private:
  bool RemoveOnDestruction;
};

// ReusablePrerequisiteModules - stands for PrerequisiteModules for which all
// the required modules are built successfully. All the module files
// are owned by the modules builder.
class ReusablePrerequisiteModules : public PrerequisiteModules {
public:
  ReusablePrerequisiteModules(
      PathRef MainFile, const GlobalCompilationDatabase &CDB,
      llvm::ArrayRef<std::string> DirectRequiredModuleNames)
      : MainFile(MainFile.str()), CDB(&CDB) {
    for (llvm::StringRef ModuleName : DirectRequiredModuleNames)
      this->DirectRequiredModuleNames.insert(ModuleName);
  }

  ReusablePrerequisiteModules(const ReusablePrerequisiteModules &Other) =
      default;
  ReusablePrerequisiteModules &
  operator=(const ReusablePrerequisiteModules &) = default;
  ReusablePrerequisiteModules(ReusablePrerequisiteModules &&) = delete;
  ReusablePrerequisiteModules
  operator=(ReusablePrerequisiteModules &&) = delete;

  ~ReusablePrerequisiteModules() override = default;

  void adjustHeaderSearchOptions(HeaderSearchOptions &Options) const override {
    // Appending all built module files.
    for (const auto &RequiredModule : RequiredModules)
      Options.PrebuiltModuleFiles.insert_or_assign(
          RequiredModule->getModuleName().str(),
          RequiredModule->getModuleFilePath().str());
  }

  std::string getAsString() const {
    std::string Result;
    llvm::raw_string_ostream OS(Result);
    for (const auto &MF : RequiredModules) {
      OS << "-fmodule-file=" << MF->getModuleName() << "="
         << MF->getModuleFilePath() << " ";
    }
    return Result;
  }

  bool canReuse(const CompilerInvocation &CI,
                llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem>) const override;

  bool isModuleUnitBuilt(llvm::StringRef ModuleName) const {
    return BuiltModuleNames.contains(ModuleName);
  }

  void recordRequiredSourceForLookup(llvm::StringRef ModuleName,
                                     PathRef RequiredSource) {
    if (RequiredSource.empty())
      return;

    auto &Sources = RequiredSourcesForLookup[ModuleName];
    std::string RequiredSourceKey = maybeCaseFoldPath(RequiredSource);
    if (llvm::none_of(Sources, [&](const std::string &RecordedSource) {
          return maybeCaseFoldPath(RecordedSource) == RequiredSourceKey;
        }))
      Sources.push_back(RequiredSource.str());
  }

  void addModuleFile(std::shared_ptr<const ModuleFile> MF) {
    BuiltModuleNames.insert(MF->getModuleName());
    recordRequiredSourceForLookup(MF->getModuleName(),
                                  MF->getRequiredSourceForLookup());
    RequiredModules.emplace_back(std::move(MF));
  }

public:
  const ModuleFile *findRequiredModule(llvm::StringRef ModuleName) const {
    auto It = llvm::find_if(RequiredModules, [&](const auto &MF) {
      return MF->getModuleName() == ModuleName;
    });
    if (It == RequiredModules.end())
      return nullptr;
    return It->get();
  }

  bool matchesBuiltModuleConfiguration(llvm::StringRef ModuleName,
                                       const CompilerInvocation *CI,
                                       llvm::StringRef ModuleSourceIdentity,
                                       llvm::StringRef CompileCommandFingerprint) const {
    const ModuleFile *MF = findRequiredModule(ModuleName);
    if (!MF)
      return false;

    if (CI)
      if (auto It = CI->getHeaderSearchOpts().PrebuiltModuleFiles.find(ModuleName);
          It != CI->getHeaderSearchOpts().PrebuiltModuleFiles.end())
        return maybeCaseFoldPath(It->second) ==
               maybeCaseFoldPath(MF->getModuleFilePath());

    return MF->getModuleSourceIdentity() == ModuleSourceIdentity &&
           MF->getCompileCommandFingerprint() == CompileCommandFingerprint;
  }

  llvm::Error
  appendStableBuildKey(llvm::raw_ostream &OS,
                       llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS) const {
    llvm::SmallVector<std::tuple<std::string, std::string, uint64_t>, 8>
        StableEntries;
    StableEntries.reserve(RequiredModules.size());
    for (const auto &RequiredModule : RequiredModules) {
      auto ModuleHash =
          getFileContentHash(RequiredModule->getModuleFilePath(), VFS);
      if (!ModuleHash)
        return llvm::createStringError(
            llvm::formatv("Failed to hash prerequisite module file {0}",
                          RequiredModule->getModuleFilePath()));
      StableEntries.emplace_back(
          RequiredModule->getModuleName().str(),
          maybeCaseFoldPath(RequiredModule->getModuleFilePath()), *ModuleHash);
    }
    llvm::sort(StableEntries);
    for (const auto &[ModuleName, ModuleFilePath, ModuleHash] : StableEntries) {
      appendStableKeyField(OS, ModuleName);
      appendStableKeyField(OS, ModuleFilePath);
      appendStableKeyField(OS, llvm::utohexstr(ModuleHash));
    }
    return llvm::Error::success();
  }

private:
  class StaticPrerequisiteModules : public PrerequisiteModules {
  public:
    explicit StaticPrerequisiteModules(
        llvm::ArrayRef<std::shared_ptr<const ModuleFile>> RequiredModules)
        : RequiredModules(RequiredModules) {}

    void adjustHeaderSearchOptions(HeaderSearchOptions &Options) const override {
      for (const auto &RequiredModule : RequiredModules)
        Options.PrebuiltModuleFiles.insert_or_assign(
            RequiredModule->getModuleName().str(),
            RequiredModule->getModuleFilePath().str());
    }

    bool canReuse(const CompilerInvocation &,
                  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem>) const override {
      return false;
    }

  private:
    llvm::ArrayRef<std::shared_ptr<const ModuleFile>> RequiredModules;
  };

  bool hasSameRequiredModules(ProjectModules &ProjectModules) const {
    llvm::StringSet<> CurrentRequiredModuleNames;
    for (llvm::StringRef ModuleName :
         ProjectModules.getRequiredModules(MainFile))
      CurrentRequiredModuleNames.insert(ModuleName);

    if (CurrentRequiredModuleNames.size() != DirectRequiredModuleNames.size())
      return false;

    for (llvm::StringRef ModuleName : DirectRequiredModuleNames.keys()) {
      if (!CurrentRequiredModuleNames.contains(ModuleName))
        return false;
    }
    return true;
  }

  bool hasSameModuleConfiguration(
      const CompilerInvocation &CI,
      llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS,
      ProjectModules &ProjectModules) const {
    const auto &CurrentPrebuiltModuleFiles =
        CI.getHeaderSearchOpts().PrebuiltModuleFiles;
    for (const auto &MF : RequiredModules) {
      if (auto It = CurrentPrebuiltModuleFiles.find(MF->getModuleName());
          It != CurrentPrebuiltModuleFiles.end()) {
        if (maybeCaseFoldPath(It->second) !=
            maybeCaseFoldPath(MF->getModuleFilePath()))
          return false;
        continue;
      }

      auto HasSameExplicitPrebuiltConfiguration = [&](PathRef RequiredSource) {
        auto Cmd = CDB->getCompileCommand(RequiredSource);
        if (!Cmd)
          return false;

        auto ValidationCI = buildCompilerInvocationForCommand(*Cmd, VFS);
        if (!ValidationCI)
          return false;

        auto It =
            ValidationCI->getHeaderSearchOpts().PrebuiltModuleFiles.find(
                MF->getModuleName());
        return It != ValidationCI->getHeaderSearchOpts().PrebuiltModuleFiles.end() &&
               maybeCaseFoldPath(It->second) ==
                   maybeCaseFoldPath(MF->getModuleFilePath());
      };

      auto HasSameSourceBackedConfiguration = [&](PathRef RequiredSource) {
        std::string ModuleUnitFileName =
            ProjectModules.getSourceForModuleName(MF->getModuleName(),
                                                  RequiredSource);
        if (ModuleUnitFileName.empty())
          return MF->getModuleSourceIdentity().empty() &&
                 MF->getCompileCommandFingerprint().empty();

        auto Cmd = CDB->getCompileCommand(ModuleUnitFileName);
        if (!Cmd)
          return false;

        if (MF->getModuleSourceIdentity() != getResolvedModuleSourceIdentity(
                                                 ModuleUnitFileName,
                                                 Cmd->Directory, VFS))
          return false;

        return MF->getCompileCommandFingerprint() ==
               getCompileCommandFingerprint(*Cmd);
      };

      auto HasSameConfiguration = [&](PathRef RequiredSource) {
        if (MF->getModuleSourceIdentity().empty() &&
            MF->getCompileCommandFingerprint().empty() &&
            HasSameExplicitPrebuiltConfiguration(RequiredSource))
          return true;
        return HasSameSourceBackedConfiguration(RequiredSource);
      };

      if (auto It = RequiredSourcesForLookup.find(MF->getModuleName());
          It != RequiredSourcesForLookup.end()) {
        if (!llvm::all_of(It->second, HasSameConfiguration))
          return false;
        continue;
      }

      PathRef RequiredSource = MF->getRequiredSourceForLookup().empty()
                                   ? MainFile
                                   : MF->getRequiredSourceForLookup();
      if (!HasSameConfiguration(RequiredSource))
        return false;
    }
    return true;
  }

  bool hasSameDirectModuleConfiguration(
      const CompilerInvocation &CI,
      llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS,
      ProjectModules &ProjectModules) const {
    for (llvm::StringRef ModuleName : DirectRequiredModuleNames.keys()) {
      const ModuleFile *MF = findRequiredModule(ModuleName);
      if (!MF)
        return false;

      if (auto It = CI.getHeaderSearchOpts().PrebuiltModuleFiles.find(ModuleName);
          It != CI.getHeaderSearchOpts().PrebuiltModuleFiles.end()) {
        if (maybeCaseFoldPath(It->second) !=
            maybeCaseFoldPath(MF->getModuleFilePath()))
          return false;
        continue;
      }

      std::string ModuleUnitFileName =
          ProjectModules.getSourceForModuleName(ModuleName, MainFile);
      if (ModuleUnitFileName.empty())
        continue;

      auto Cmd = CDB->getCompileCommand(ModuleUnitFileName);
      if (!Cmd)
        return false;

      if (MF->getModuleSourceIdentity() != getResolvedModuleSourceIdentity(
                                             ModuleUnitFileName,
                                             Cmd->Directory, VFS))
        return false;
      if (MF->getCompileCommandFingerprint() !=
          getCompileCommandFingerprint(*Cmd))
        return false;
    }
    return true;
  }

  bool canReuseSourceBackedModule(
      const ModuleFile &MF, size_t Index, const CompilerInvocation &ImporterCI,
      llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS,
      ProjectModules &ProjectModules) const {
    PathRef RequiredSource = MF.getRequiredSourceForLookup().empty()
                                 ? MainFile
                                 : MF.getRequiredSourceForLookup();
    std::string ModuleUnitFileName =
        ProjectModules.getSourceForModuleName(MF.getModuleName(), RequiredSource);
    if (ModuleUnitFileName.empty())
      return false;

    auto Cmd = CDB->getCompileCommand(ModuleUnitFileName);
    if (!Cmd)
      return false;

    auto ValidationCI = buildCompilerInvocationForCommand(*Cmd, VFS);
    if (!ValidationCI)
      return false;
    applyModuleBuildInvocationSettings(*ValidationCI);

    // Revalidate the BMI against the prerequisite prefix that existed when it
    // was originally built, not the whole final reusable set.
    StaticPrerequisiteModules BuiltBeforeCurrent(
        llvm::ArrayRef<std::shared_ptr<const ModuleFile>>(RequiredModules)
            .take_front(Index));
    if (!IsModuleFileUpToDate(MF.getModuleFilePath(), BuiltBeforeCurrent, VFS,
                              ValidationCI.get()))
      return false;

    if (!hasCompatibleImporterConfiguration(*ValidationCI, ImporterCI))
      return false;

    // Also validate that the current importer can still load this BMI. Source-
    // backed modules have fast manifests, so skip that shortcut here and let
    // ASTReader catch importer-side target/configuration mismatches.
    return IsModuleFileUpToDate(MF.getModuleFilePath(), BuiltBeforeCurrent, VFS,
                                &ImporterCI,
                                /*CheckStoredContextHash=*/false,
                                /*AllowFastManifestValidation=*/false);
  }

  llvm::SmallVector<std::shared_ptr<const ModuleFile>, 8> RequiredModules;
  std::string MainFile;
  const GlobalCompilationDatabase *CDB = nullptr;
  llvm::StringSet<> DirectRequiredModuleNames;
  // A helper class to speedup the query if a module is built.
  llvm::StringSet<> BuiltModuleNames;
  llvm::StringMap<llvm::SmallVector<std::string>> RequiredSourcesForLookup;
};

bool IsModuleFileUpToDate(PathRef ModuleFilePath,
                          const PrerequisiteModules &RequisiteModules,
                          llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS,
                          const CompilerInvocation *CI,
                          bool CheckStoredContextHash,
                          bool AllowFastManifestValidation) {
  auto ModuleStatus = VFS->status(ModuleFilePath);
  if (!ModuleStatus || !ModuleStatus->isRegularFile()) {
    return false;
  }
  std::optional<CompilerInvocation> EffectiveCI;
  if (CI) {
    EffectiveCI.emplace(*CI);
    applyModuleBuildInvocationSettings(*EffectiveCI);
    RequisiteModules.adjustHeaderSearchOptions(
        EffectiveCI->getHeaderSearchOpts());
    EffectiveCI->getFrontendOpts().OutputFile = ModuleFilePath.str();

    if (CheckStoredContextHash) {
      auto StoredContextHash = readModuleContextHashFile(ModuleFilePath, VFS);
      if (!StoredContextHash)
        AllowFastManifestValidation = false;
      else if (*StoredContextHash != EffectiveCI->computeContextHash())
        return false;
    }
  }
  auto FastManifestInputs = AllowFastManifestValidation
                                ? readModuleInputManifestFile(ModuleFilePath,
                                                              VFS)
                                : std::nullopt;
  if (FastManifestInputs) {
    auto CurrentModuleHash = getFileContentHash(ModuleFilePath, VFS);
    if (!CurrentModuleHash ||
        *CurrentModuleHash != FastManifestInputs->ModuleFileHash)
      return false;

    for (const auto &Input : FastManifestInputs->Inputs) {
      auto Status = VFS->status(Input.Path);
      if (!Status || !Status->isRegularFile()) {
        return false;
      }

      if (Input.ContentHash == 0) {
        return false;
      }

      auto Buffer = VFS->getBufferForFile(Input.Path);
      if (!Buffer) {
        return false;
      }
      auto CurrentHash =
          static_cast<uint64_t>(llvm::xxh3_64bits(Buffer.get()->getBuffer()));
      if (Input.ContentHash != CurrentHash) {
        return false;
      }
    }

    HeaderSearchOptions CurrentHS;
    RequisiteModules.adjustHeaderSearchOptions(CurrentHS);
    for (const auto &Import : FastManifestInputs->Imports) {
      if (Import.ContentHash == 0)
        return false;

      if (auto It = CurrentHS.PrebuiltModuleFiles.find(Import.ModuleName);
          It != CurrentHS.PrebuiltModuleFiles.end() &&
          maybeCaseFoldPath(It->second) != maybeCaseFoldPath(Import.Path))
        return false;

      auto CurrentImportHash = getFileContentHash(Import.Path, VFS);
      if (!CurrentImportHash || *CurrentImportHash != Import.ContentHash)
        return false;
    }
    return true;
  }

  HeaderSearchOptions HSOpts;
  LangOptions LangOpts;
  PreprocessorOptions PPOpts;
  if (EffectiveCI) {
    HSOpts = EffectiveCI->getHeaderSearchOpts();
    LangOpts = EffectiveCI->getLangOpts();
    PPOpts = EffectiveCI->getPreprocessorOpts();
  }
  else
    RequisiteModules.adjustHeaderSearchOptions(HSOpts);
  HSOpts.ModulesValidateSystemHeaders = true;
  HSOpts.ForceCheckCXX20ModulesInputFiles = true;
  HSOpts.ValidateASTInputFilesContent = true;

  clang::clangd::IgnoreDiagnostics IgnoreDiags;
  DiagnosticOptions DiagOpts;
  IntrusiveRefCntPtr<DiagnosticsEngine> Diags =
      CompilerInstance::createDiagnostics(*VFS, DiagOpts, &IgnoreDiags,
                                          /*ShouldOwnClient=*/false);

  // In clang's driver, we suppress ODR checks in GMF while building modules.
  // Keep validation aligned with that mode to avoid mismatched acceptance.
  LangOpts.SkipODRCheckInGMF = true;

  FileManager FileMgr(FileSystemOptions(), VFS);
  SourceManager SourceMgr(*Diags, FileMgr);
  HeaderSearch HeaderInfo(HSOpts, SourceMgr, *Diags, LangOpts,
                          /*Target=*/nullptr);
  TrivialModuleLoader ModuleLoader;
  Preprocessor PP(PPOpts, *Diags, LangOpts, SourceMgr, HeaderInfo,
                  ModuleLoader);

  std::shared_ptr<ModuleCache> ModCache = createCrossProcessModuleCache();
  PCHContainerOperations PCHOperations;
  CodeGenOptions CodeGenOpts;
  ASTReader Reader(PP, *ModCache, /*ASTContext=*/nullptr,
                   PCHOperations.getRawReader(), CodeGenOpts, {},
                   /*isysroot=*/"",
                   DisableValidationForModuleKind::None,
                   /*AllowASTWithCompilerErrors=*/false,
                   /*AllowConfigurationMismatch=*/false,
                   HSOpts.ModulesValidateSystemHeaders,
                   HSOpts.ModulesForceValidateUserHeaders,
                   HSOpts.ValidateASTInputFilesContent);

  // We don't need any listener here. By default it will use a validator
  // listener.
  Reader.setListener(nullptr);

  const unsigned ValidationCaps =
      ASTReader::ARR_OutOfDate | ASTReader::ARR_Missing |
      ASTReader::ARR_ConfigurationMismatch |
      ASTReader::ARR_TreatModuleWithErrorsAsOutOfDate;
  if (Reader.ReadAST(ModuleFilePath, serialization::MK_MainFile,
                     SourceLocation(), ValidationCaps) != ASTReader::Success) {
    return false;
  }

  bool UpToDate = true;
  Reader.getModuleManager().visit([&](serialization::ModuleFile &MF) -> bool {
    Reader.visitInputFiles(
        MF, /*IncludeSystem=*/true, /*Complain=*/false,
        [&](const serialization::InputFile &IF, bool isSystem) {
          if (!IF.getFile() || IF.isOutOfDate()) {
            UpToDate = false;
          }
        });
    return !UpToDate;
  });
  return UpToDate;
}

bool IsModuleFileFullyUpToDate(
    PathRef ModuleFilePath, const PrerequisiteModules &RequisiteModules,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS,
    const CompilerInvocation *CI) {
  if (!IsModuleFileUpToDate(ModuleFilePath, RequisiteModules, VFS, CI))
    return false;
  if (!CI)
    return true;
  return IsModuleFileUpToDate(ModuleFilePath, RequisiteModules, VFS, CI,
                              /*CheckStoredContextHash=*/false,
                              /*AllowFastManifestValidation=*/false);
}

llvm::Expected<std::string> getStableModuleVariantFingerprint(
    llvm::StringRef ModuleSourceIdentity,
    llvm::StringRef CompileCommandFingerprint,
    llvm::StringRef ModuleSourceContents,
    const ReusablePrerequisiteModules &BuiltModuleFiles,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS) {
  llvm::SmallString<256> StableKey;
  llvm::raw_svector_ostream OS(StableKey);
  appendStableKeyField(OS, maybeCaseFoldPath(ModuleSourceIdentity));
  appendStableKeyField(OS, CompileCommandFingerprint);
  appendStableKeyField(
      OS, llvm::utohexstr(llvm::xxh3_64bits(ModuleSourceContents)));
  if (llvm::Error Err = BuiltModuleFiles.appendStableBuildKey(OS, VFS))
    return std::move(Err);
  return llvm::utohexstr(llvm::xxh3_64bits(OS.str()));
}

std::optional<ModuleInputManifest>
collectModuleInputManifest(PathRef ModuleFilePath,
                           const PrerequisiteModules &RequisiteModules,
                           llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS,
                           const CompilerInvocation &CI) {
  HeaderSearchOptions HSOpts = CI.getHeaderSearchOpts();
  LangOptions LangOpts = CI.getLangOpts();
  PreprocessorOptions PPOpts = CI.getPreprocessorOpts();

  RequisiteModules.adjustHeaderSearchOptions(HSOpts);
  HSOpts.ModulesValidateSystemHeaders = true;
  HSOpts.ForceCheckCXX20ModulesInputFiles = true;
  HSOpts.ValidateASTInputFilesContent = true;
  LangOpts.SkipODRCheckInGMF = true;

  clang::clangd::IgnoreDiagnostics IgnoreDiags;
  DiagnosticOptions DiagOpts;
  IntrusiveRefCntPtr<DiagnosticsEngine> Diags =
      CompilerInstance::createDiagnostics(*VFS, DiagOpts, &IgnoreDiags,
                                          /*ShouldOwnClient=*/false);
  FileManager FileMgr(FileSystemOptions(), VFS);
  SourceManager SourceMgr(*Diags, FileMgr);
  HeaderSearch HeaderInfo(HSOpts, SourceMgr, *Diags, LangOpts,
                          /*Target=*/nullptr);
  TrivialModuleLoader ModuleLoader;
  Preprocessor PP(PPOpts, *Diags, LangOpts, SourceMgr, HeaderInfo,
                  ModuleLoader);

  std::shared_ptr<ModuleCache> ModCache = createCrossProcessModuleCache();
  PCHContainerOperations PCHOperations;
  CodeGenOptions CodeGenOpts;
  ASTReader Reader(PP, *ModCache, /*ASTContext=*/nullptr,
                   PCHOperations.getRawReader(), CodeGenOpts, {},
                   /*isysroot=*/"",
                   DisableValidationForModuleKind::None,
                   /*AllowASTWithCompilerErrors=*/false,
                   /*AllowConfigurationMismatch=*/false,
                   HSOpts.ModulesValidateSystemHeaders,
                   HSOpts.ModulesForceValidateUserHeaders,
                   HSOpts.ValidateASTInputFilesContent);
  Reader.setListener(nullptr);

  const unsigned ValidationCaps =
      ASTReader::ARR_OutOfDate | ASTReader::ARR_Missing |
      ASTReader::ARR_ConfigurationMismatch |
      ASTReader::ARR_TreatModuleWithErrorsAsOutOfDate;
  if (Reader.ReadAST(ModuleFilePath, serialization::MK_MainFile,
                     SourceLocation(), ValidationCaps) != ASTReader::Success)
    return std::nullopt;

  llvm::StringMap<ModuleInputStamp> UniqueInputs;
  llvm::StringMap<ModuleImportStamp> UniqueImports;
  const std::string RootPCMKey = maybeCaseFoldPath(ModuleFilePath);
  Reader.getModuleManager().visit([&](serialization::ModuleFile &MF) -> bool {
    if (!MF.ModuleName.empty() && maybeCaseFoldPath(MF.FileName) != RootPCMKey) {
      auto [It, Inserted] = UniqueImports.try_emplace(MF.ModuleName);
      if (Inserted) {
        It->second.ModuleName = MF.ModuleName;
        It->second.Path = MF.FileName;
        if (auto ImportHash = getFileContentHash(MF.FileName, VFS))
          It->second.ContentHash = *ImportHash;
      }
    }

    std::vector<serialization::InputFileInfo> InputInfos;
    Reader.visitInputFileInfos(MF, /*IncludeSystem=*/true,
                               [&](const serialization::InputFileInfo &IFI,
                                   bool) { InputInfos.push_back(IFI); });
    unsigned Index = 0;
    Reader.visitInputFiles(
        MF, /*IncludeSystem=*/true, /*Complain=*/false,
        [&](const serialization::InputFile &IF, bool) {
          if (Index >= InputInfos.size()) {
            ++Index;
            return;
          }
          const auto &Info = InputInfos[Index++];
          auto File = IF.getFile();
          if (!File)
            return;

          std::string Key = maybeCaseFoldPath(File->getName());
          auto [It, Inserted] = UniqueInputs.try_emplace(Key);
          if (!Inserted)
            return;

          It->second.Path = File->getName().str();
          It->second.StoredSize = static_cast<uint64_t>(Info.StoredSize);
          It->second.StoredTime = static_cast<int64_t>(Info.StoredTime);
          It->second.ContentHash = Info.ContentHash;
        });
    return false;
  });

  ModuleInputManifest Manifest;
  Manifest.Inputs.reserve(UniqueInputs.size());
  for (const auto &Entry : UniqueInputs)
    Manifest.Inputs.push_back(Entry.second);
  Manifest.Imports.reserve(UniqueImports.size());
  for (const auto &Entry : UniqueImports)
    Manifest.Imports.push_back(Entry.second);
  return Manifest;
}

/// Build a module file for module with `ModuleName`. The information of built
/// module file are stored in \param BuiltModuleFiles.
llvm::Expected<std::shared_ptr<BuiltModuleFile>>
buildModuleFile(llvm::StringRef ModuleName, PathRef ModuleUnitFileName,
                PathRef RequiredSourceForLookup,
                StringRef ModuleSourceIdentity,
                StringRef CompileCommandFingerprint,
                const GlobalCompilationDatabase &CDB, const ThreadsafeFS &TFS,
                const ReusablePrerequisiteModules &BuiltModuleFiles) {
  // Try cheap operation earlier to boil-out cheaply if there are problems.
  auto Cmd = CDB.getCompileCommand(ModuleUnitFileName);
  if (!Cmd)
    return llvm::createStringError(
        llvm::formatv("No compile command for {0}", ModuleUnitFileName));

  ParseInputs Inputs;
  Inputs.TFS = &TFS;
  Inputs.CompileCommand = std::move(*Cmd);

  auto FS = Inputs.TFS->view(Inputs.CompileCommand.Directory);
  auto Buf = FS->getBufferForFile(Inputs.CompileCommand.Filename);
  if (!Buf)
    return llvm::createStringError("Failed to create buffer");

  auto StableModuleVariantFingerprint = getStableModuleVariantFingerprint(
      ModuleSourceIdentity, CompileCommandFingerprint,
      Buf.get()->getBuffer(), BuiltModuleFiles, TFS.view(std::nullopt));
  if (!StableModuleVariantFingerprint)
    return StableModuleVariantFingerprint.takeError();

  llvm::SmallString<256> StableModuleFilesPath = getStableModuleFilesPath(
      ModuleUnitFileName, *StableModuleVariantFingerprint);
  std::string StableOutputPath =
      getModuleFilePath(ModuleName, StableModuleFilesPath);
  std::string TemporaryModuleFilesPath =
      getTemporaryModuleFilesPath(StableModuleFilesPath);
  std::string TemporaryOutputPath =
      getModuleFilePath(ModuleName, TemporaryModuleFilesPath);
  Inputs.CompileCommand.Output = TemporaryOutputPath;

  IgnoreDiagnostics IgnoreDiags;
  auto CI = buildCompilerInvocation(Inputs, IgnoreDiags);
  if (!CI)
    return llvm::createStringError("Failed to build compiler invocation");

  CI->getFrontendOpts().OutputFile = StableOutputPath;
  if (IsModuleFileUpToDate(StableOutputPath, BuiltModuleFiles,
                           TFS.view(std::nullopt), CI.get()) &&
      IsModuleFileUpToDate(StableOutputPath, BuiltModuleFiles,
                           TFS.view(std::nullopt), CI.get(),
                           /*CheckStoredContextHash=*/false,
                           /*AllowFastManifestValidation=*/false)) {
    return BuiltModuleFile::make(ModuleName, StableOutputPath,
                                 ModuleSourceIdentity,
                                 CompileCommandFingerprint,
                                 RequiredSourceForLookup,
                                 /*RemoveOnDestruction=*/false);
  }

  applyModuleBuildInvocationSettings(*CI);
  CompilerInvocation ValidationCI(*CI);

  BuiltModuleFiles.adjustHeaderSearchOptions(CI->getHeaderSearchOpts());
  const std::string ModuleContextHash = CI->computeContextHash();

  CI->getFrontendOpts().OutputFile = Inputs.CompileCommand.Output;
  llvm::scope_exit CleanupTemporaryArtifacts([&] {
    removeModuleArtifacts(Inputs.CompileCommand.Output);
  });
  auto Clang =
      prepareCompilerInstance(std::move(CI), /*Preamble=*/nullptr,
                              std::move(*Buf), std::move(FS), IgnoreDiags);
  if (!Clang)
    return llvm::createStringError("Failed to prepare compiler instance");

  GenerateReducedModuleInterfaceAction Action;
  Clang->ExecuteAction(Action);

  if (Clang->getDiagnostics().hasErrorOccurred()) {
    std::string Cmds;
    for (const auto &Arg : Inputs.CompileCommand.CommandLine) {
      if (!Cmds.empty())
        Cmds += " ";
      Cmds += Arg;
    }

    clangd::vlog("Failed to compile {0} with command: {1}", ModuleUnitFileName,
                 Cmds);

    std::string BuiltModuleFilesStr = BuiltModuleFiles.getAsString();
    if (!BuiltModuleFilesStr.empty())
      clangd::vlog("The actual used module files built by clangd is {0}",
                   BuiltModuleFilesStr);

    return llvm::createStringError(
        llvm::formatv("Failed to compile {0}. Use '--log=verbose' to view "
                      "detailed failure reasons. It is helpful to use "
                      "'--debug-modules-builder' flag to keep the clangd's "
                      "built module files to reproduce the failure for "
                      "debugging. Remember to remove them after debugging.",
                      ModuleUnitFileName));
  }

  writeModuleContextHashFile(Inputs.CompileCommand.Output, ModuleContextHash);
  if (auto Manifest = collectModuleInputManifest(
          Inputs.CompileCommand.Output, BuiltModuleFiles,
          TFS.view(std::nullopt), Clang->getInvocation())) {
    if (auto ModuleFileHash = getFileContentHash(Inputs.CompileCommand.Output,
                                                 TFS.view(std::nullopt))) {
      Manifest->ModuleFileHash = *ModuleFileHash;
      writeModuleInputManifestFile(Inputs.CompileCommand.Output, *Manifest);
    }
  }

  auto ReusePublishedStableOutput = [&]() {
    return BuiltModuleFile::make(ModuleName, StableOutputPath,
                                 ModuleSourceIdentity,
                                 CompileCommandFingerprint,
                                 RequiredSourceForLookup,
                                 /*RemoveOnDestruction=*/false);
  };
  auto ReuseTemporaryOutput = [&]() {
    CleanupTemporaryArtifacts.release();
    return BuiltModuleFile::make(ModuleName, TemporaryOutputPath,
                                 ModuleSourceIdentity,
                                 CompileCommandFingerprint,
                                 RequiredSourceForLookup,
                                 /*RemoveOnDestruction=*/true);
  };

  if (IsModuleFileFullyUpToDate(StableOutputPath, BuiltModuleFiles,
                                TFS.view(std::nullopt), &ValidationCI))
    return ReusePublishedStableOutput();

  std::error_code PublishEC =
      llvm::sys::fs::rename(TemporaryModuleFilesPath, StableModuleFilesPath);
  if (!PublishEC) {
    CleanupTemporaryArtifacts.release();
    return ReusePublishedStableOutput();
  }

  if (llvm::sys::fs::exists(StableOutputPath) &&
      IsModuleFileFullyUpToDate(StableOutputPath, BuiltModuleFiles,
                                TFS.view(std::nullopt), &ValidationCI))
    return ReusePublishedStableOutput();

  vlog("Failed to publish shared module cache directory {0} for module {1}: "
       "{2}. Keeping builder-local artifact {3}",
       StableModuleFilesPath, ModuleName, PublishEC.message(),
       TemporaryOutputPath);
  return ReuseTemporaryOutput();
}

bool ReusablePrerequisiteModules::canReuse(
    const CompilerInvocation &CI,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS) const {
  auto ProjectModules = CDB->getProjectModules(MainFile);
  if (!ProjectModules)
    return false;

  if (!hasSameRequiredModules(*ProjectModules))
    return false;

  if (!hasSameModuleConfiguration(CI, VFS, *ProjectModules))
    return false;

  // A direct import may have been satisfied transitively when this reusable set
  // was built. Recheck the current main-file lookup so a newly-resolved direct
  // module mapping can't silently pin the old BMI.
  if (!hasSameDirectModuleConfiguration(CI, VFS, *ProjectModules))
    return false;

  if (RequiredModules.empty())
    return true;

  for (size_t I = 0; I < RequiredModules.size(); ++I) {
    const auto &MF = *RequiredModules[I];

    if (MF.getModuleSourceIdentity().empty()) {
      PathRef RequiredSource = MF.getRequiredSourceForLookup().empty()
                                   ? MainFile
                                   : MF.getRequiredSourceForLookup();
      std::string ModuleUnitFileName =
          ProjectModules->getSourceForModuleName(MF.getModuleName(),
                                                 RequiredSource);
      if (!ModuleUnitFileName.empty()) {
        auto Cmd = CDB->getCompileCommand(ModuleUnitFileName);
        if (!Cmd)
          return false;

        auto ValidationCI = buildCompilerInvocationForCommand(*Cmd, VFS);
        if (!ValidationCI)
          return false;
        applyModuleBuildInvocationSettings(*ValidationCI);

        if (!hasCompatibleImporterConfiguration(*ValidationCI, CI))
          return false;
      }

      if (!IsModuleFileUpToDate(MF.getModuleFilePath(), *this, VFS, &CI,
                                /*CheckStoredContextHash=*/true,
                                /*AllowFastManifestValidation=*/false))
        return false;
      continue;
    }

    if (!canReuseSourceBackedModule(MF, I, CI, VFS, *ProjectModules))
      return false;
  }
  return true;
}

class ModuleFileCache {
public:
  std::shared_ptr<const ModuleFile> getModule(StringRef ModuleName,
                                              PathRef ModuleUnitSourcePath);

  void add(StringRef ModuleName, PathRef ModuleUnitSourcePath,
           std::shared_ptr<const ModuleFile> ModuleFile) {
    std::lock_guard<std::mutex> Lock(ModuleFilesMutex);

    ModuleFiles[ModuleName][maybeCaseFoldPath(ModuleUnitSourcePath)] =
        std::move(ModuleFile);
  }

  void remove(StringRef ModuleName, PathRef ModuleUnitSourcePath);

private:
  llvm::StringMap<llvm::StringMap<std::shared_ptr<const ModuleFile>>> ModuleFiles;
  // Mutex to guard accesses to ModuleFiles.
  std::mutex ModuleFilesMutex;
};

std::shared_ptr<const ModuleFile>
ModuleFileCache::getModule(StringRef ModuleName, PathRef ModuleUnitSourcePath) {
  std::lock_guard<std::mutex> Lock(ModuleFilesMutex);

  auto It = ModuleFiles.find(ModuleName);
  if (It == ModuleFiles.end())
    return nullptr;

  auto SourcePathKey = maybeCaseFoldPath(ModuleUnitSourcePath);
  auto SourceIt = It->second.find(SourcePathKey);
  if (SourceIt == It->second.end())
    return nullptr;
  return SourceIt->second;
}

void ModuleFileCache::remove(StringRef ModuleName,
                             PathRef ModuleUnitSourcePath) {
  std::lock_guard<std::mutex> Lock(ModuleFilesMutex);

  auto It = ModuleFiles.find(ModuleName);
  if (It == ModuleFiles.end())
    return;

  It->second.erase(maybeCaseFoldPath(ModuleUnitSourcePath));
  if (It->second.empty())
    ModuleFiles.erase(It);
}

ModuleFileCache &getGlobalModuleFileCache() {
  static ModuleFileCache Cache;
  return Cache;
}

class PrerequisiteModulesCache {
public:
  std::shared_ptr<const ReusablePrerequisiteModules>
  getReusable(PathRef MainFile, const CompilerInvocation &CI,
              llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS) {
    std::vector<std::shared_ptr<const ReusablePrerequisiteModules>> Candidates;
    {
      std::lock_guard<std::mutex> Lock(CacheMutex);
      auto It = Cache.find(maybeCaseFoldPath(MainFile));
      if (It == Cache.end())
        return nullptr;
      Candidates.assign(It->second.begin(), It->second.end());
    }

    for (auto It = Candidates.rbegin(); It != Candidates.rend(); ++It)
      if ((*It)->canReuse(CI, VFS))
        return *It;
    return nullptr;
  }

  void add(PathRef MainFile,
           std::shared_ptr<const ReusablePrerequisiteModules> Modules) {
    std::lock_guard<std::mutex> Lock(CacheMutex);
    auto &Entries = Cache[maybeCaseFoldPath(MainFile)];
    Entries.push_back(std::move(Modules));
    if (Entries.size() > 4)
      Entries.erase(Entries.begin(),
                    Entries.begin() + (Entries.size() - 4));
  }

private:
  std::mutex CacheMutex;
  llvm::StringMap<
      llvm::SmallVector<std::shared_ptr<const ReusablePrerequisiteModules>, 2>>
      Cache;
};

class ModuleNameToSourceCache {
public:
  std::string getSourceForModuleName(llvm::StringRef ModuleName,
                                     PathRef RequiredSrcFile) {
    std::lock_guard<std::mutex> Lock(CacheMutex);

    auto It = ModuleNameToSourceCache.find(ModuleName);
    if (It == ModuleNameToSourceCache.end())
      return "";

    auto RequiredSrcKey = maybeCaseFoldPath(RequiredSrcFile);
    auto RequiredSrcIt = It->second.find(RequiredSrcKey);
    if (RequiredSrcIt != It->second.end())
      return RequiredSrcIt->second;

    return "";
  }

  void addEntry(llvm::StringRef ModuleName, PathRef RequiredSrcFile,
                PathRef Source) {
    std::lock_guard<std::mutex> Lock(CacheMutex);
    ModuleNameToSourceCache[ModuleName][maybeCaseFoldPath(RequiredSrcFile)] =
        Source.str();
  }

  void eraseEntry(llvm::StringRef ModuleName, PathRef RequiredSrcFile) {
    std::lock_guard<std::mutex> Lock(CacheMutex);
    auto It = ModuleNameToSourceCache.find(ModuleName);
    if (It == ModuleNameToSourceCache.end())
      return;

    It->second.erase(maybeCaseFoldPath(RequiredSrcFile));
    if (It->second.empty())
      ModuleNameToSourceCache.erase(It);
  }

private:
  std::mutex CacheMutex;
  llvm::StringMap<llvm::StringMap<std::string>> ModuleNameToSourceCache;
};

class CachingProjectModules : public ProjectModules {
public:
  CachingProjectModules(std::unique_ptr<ProjectModules> MDB,
                        ModuleNameToSourceCache &Cache)
      : MDB(std::move(MDB)), Cache(Cache) {
    assert(this->MDB && "CachingProjectModules should only be created with a "
                        "valid underlying ProjectModules");
  }

  std::vector<std::string> getRequiredModules(PathRef File) override {
    return MDB->getRequiredModules(File);
  }

  std::string getModuleNameForSource(PathRef File) override {
    return MDB->getModuleNameForSource(File);
  }

  std::string getSourceForModuleName(llvm::StringRef ModuleName,
                                     PathRef RequiredSrcFile) override {
    auto &RefreshedRequiredSources = RefreshedLookupContexts[ModuleName];
    std::string RequiredSrcKey = maybeCaseFoldPath(RequiredSrcFile);
    if (!RefreshedRequiredSources.insert(RequiredSrcKey).second)
      if (std::string CachedResult =
              Cache.getSourceForModuleName(ModuleName, RequiredSrcFile);
          !CachedResult.empty())
        return CachedResult;

    auto Result = MDB->getSourceForModuleName(ModuleName, RequiredSrcFile);
    Cache.addEntry(ModuleName, RequiredSrcFile, Result);

    return Result;
  }

private:
  std::unique_ptr<ProjectModules> MDB;
  ModuleNameToSourceCache &Cache;
  llvm::StringMap<llvm::StringSet<>> RefreshedLookupContexts;
};

struct RequiredModuleDesc {
  std::string Name;
  std::string RequiredSource;
};

/// Collect the directly and indirectly required module names for \param
/// ModuleName in topological order. The \param ModuleName is guaranteed to
/// be the last element in \param ModuleNames.
llvm::SmallVector<RequiredModuleDesc>
getAllRequiredModules(PathRef RequiredSource, CachingProjectModules &MDB,
                      StringRef ModuleName) {
  llvm::SmallVector<RequiredModuleDesc> ModuleNames;
  llvm::StringMap<llvm::StringSet<>> SeenLookupContexts;

  auto VisitDeps = [&](StringRef ModuleName, PathRef RequiredSource,
                       auto Visitor) -> void {
    std::string RequiredSourceKey = maybeCaseFoldPath(RequiredSource);
    if (!SeenLookupContexts[ModuleName].insert(RequiredSourceKey).second)
      return;

    std::string SourceForModule =
        MDB.getSourceForModuleName(ModuleName, RequiredSource);
    for (StringRef RequiredModuleName : MDB.getRequiredModules(SourceForModule))
      Visitor(RequiredModuleName, SourceForModule, Visitor);

    ModuleNames.push_back({ModuleName.str(), std::string(RequiredSource)});
  };
  VisitDeps(ModuleName, RequiredSource, VisitDeps);

  return ModuleNames;
}

} // namespace

class ModulesBuilder::ModulesBuilderImpl {
public:
  ModulesBuilderImpl(const GlobalCompilationDatabase &CDB)
      : CDB(CDB), Cache(getGlobalModuleFileCache()) {}

  ModuleNameToSourceCache &getProjectModulesCache() {
    return ProjectModulesCache;
  }
  const GlobalCompilationDatabase &getCDB() const { return CDB; }
  PrerequisiteModulesCache &getPrerequisiteModulesCache() {
    return PrerequisiteCache;
  }

  llvm::Error
  getOrBuildModuleFile(PathRef RequiredSource, StringRef ModuleName,
                       const ThreadsafeFS &TFS, CachingProjectModules &MDB,
                       ReusablePrerequisiteModules &BuiltModuleFiles);

private:
  const GlobalCompilationDatabase &CDB;
  /// Try to get prebuilt module files from the compilation database.
  void getPrebuiltModuleFile(StringRef ModuleName, PathRef ModuleUnitFileName,
                             const ThreadsafeFS &TFS,
                             ReusablePrerequisiteModules &BuiltModuleFiles);
  /// Try to reuse an explicitly configured prebuilt module mapping from the
  /// requiring source's compile command.
  bool getExplicitPrebuiltModuleFile(PathRef RequiredSource,
                                     StringRef ModuleName,
                                     const ThreadsafeFS &TFS,
                                     ReusablePrerequisiteModules &BuiltModuleFiles);

  ModuleFileCache &Cache;
  PrerequisiteModulesCache PrerequisiteCache;
  ModuleNameToSourceCache ProjectModulesCache;
};

void ModulesBuilder::ModulesBuilderImpl::getPrebuiltModuleFile(
    StringRef ModuleName, PathRef ModuleUnitFileName, const ThreadsafeFS &TFS,
    ReusablePrerequisiteModules &BuiltModuleFiles) {
  auto Cmd = getCDB().getCompileCommand(ModuleUnitFileName);
  if (!Cmd)
    return;

  ParseInputs Inputs;
  Inputs.TFS = &TFS;
  Inputs.CompileCommand = std::move(*Cmd);

  IgnoreDiagnostics IgnoreDiags;
  auto CI = buildCompilerInvocation(Inputs, IgnoreDiags);
  if (!CI)
    return;

  // We don't need to check if the module files are in ModuleCache or adding
  // them to the module cache. As even if the module files are in the module
  // cache, we still need to validate them. And it looks not helpful to add them
  // to the module cache, since we may always try to get the prebuilt module
  // files before building the module files by ourselves.
  for (auto &[ModuleName, ModuleFilePath] :
       CI->getHeaderSearchOpts().PrebuiltModuleFiles) {
    if (BuiltModuleFiles.isModuleUnitBuilt(ModuleName))
      continue;

    if (IsModuleFileFullyUpToDate(ModuleFilePath, BuiltModuleFiles,
                                  TFS.view(std::nullopt), CI.get())) {
      log("Reusing prebuilt module file {0} of module {1} for {2}",
          ModuleFilePath, ModuleName, ModuleUnitFileName);
      BuiltModuleFiles.addModuleFile(
          PrebuiltModuleFile::make(ModuleName, ModuleFilePath,
                                   /*ModuleSourceIdentity=*/"",
                                   /*CompileCommandHash=*/"",
                                   /*RequiredSourceForLookup=*/ModuleUnitFileName));
    }
  }
}

bool ModulesBuilder::ModulesBuilderImpl::getExplicitPrebuiltModuleFile(
    PathRef RequiredSource, StringRef ModuleName, const ThreadsafeFS &TFS,
    ReusablePrerequisiteModules &BuiltModuleFiles) {
  auto Cmd = getCDB().getCompileCommand(RequiredSource);
  if (!Cmd)
    return false;

  ParseInputs Inputs;
  Inputs.TFS = &TFS;
  Inputs.CompileCommand = std::move(*Cmd);

  IgnoreDiagnostics IgnoreDiags;
  auto CI = buildCompilerInvocation(Inputs, IgnoreDiags);
  if (!CI)
    return false;

  auto It = CI->getHeaderSearchOpts().PrebuiltModuleFiles.find(ModuleName);
  if (It == CI->getHeaderSearchOpts().PrebuiltModuleFiles.end())
    return false;

  if (!IsModuleFileFullyUpToDate(It->second, BuiltModuleFiles,
                                 TFS.view(std::nullopt), CI.get()))
    return false;

  log("Reusing explicit prebuilt module file {0} of module {1} for {2}",
      It->second, ModuleName, RequiredSource);
  BuiltModuleFiles.addModuleFile(PrebuiltModuleFile::make(
      ModuleName, It->second,
      /*ModuleSourceIdentity=*/"",
      /*CompileCommandHash=*/"",
      /*RequiredSourceForLookup=*/RequiredSource));
  return true;
}

llvm::Error ModulesBuilder::ModulesBuilderImpl::getOrBuildModuleFile(
    PathRef RequiredSource, StringRef ModuleName, const ThreadsafeFS &TFS,
    CachingProjectModules &MDB, ReusablePrerequisiteModules &BuiltModuleFiles) {
  if (BuiltModuleFiles.isModuleUnitBuilt(ModuleName)) {
    if (auto Cmd = getCDB().getCompileCommand(RequiredSource)) {
      auto ValidationCI =
          buildCompilerInvocationForCommand(*Cmd, TFS.view(std::nullopt));
      if (ValidationCI) {
        if (auto It =
                ValidationCI->getHeaderSearchOpts().PrebuiltModuleFiles.find(
                    ModuleName);
            It != ValidationCI->getHeaderSearchOpts().PrebuiltModuleFiles.end() &&
            IsModuleFileFullyUpToDate(It->second, BuiltModuleFiles,
                                      TFS.view(std::nullopt),
                                      ValidationCI.get())) {
          if (!BuiltModuleFiles.matchesBuiltModuleConfiguration(
                  ModuleName, ValidationCI.get(),
                  /*ModuleSourceIdentity=*/"",
                  /*CompileCommandFingerprint=*/""))
            return llvm::createStringError(llvm::formatv(
                "Conflicting module lookup for module {0}", ModuleName));
          BuiltModuleFiles.recordRequiredSourceForLookup(ModuleName,
                                                         RequiredSource);
          return llvm::Error::success();
        }
      }
    }

    std::string ModuleUnitFileName =
        MDB.getSourceForModuleName(ModuleName, RequiredSource);
    if (ModuleUnitFileName.empty())
      return llvm::Error::success();

    auto Config = getModuleLookupConfiguration(ModuleUnitFileName, getCDB(), TFS);
    if (!BuiltModuleFiles.matchesBuiltModuleConfiguration(
            ModuleName, Config.CI.get(), Config.SourceIdentity,
            Config.CommandFingerprint))
      return llvm::createStringError(llvm::formatv(
          "Conflicting module lookup for module {0}", ModuleName));
    BuiltModuleFiles.recordRequiredSourceForLookup(ModuleName, RequiredSource);
    return llvm::Error::success();
  }

  if (getExplicitPrebuiltModuleFile(RequiredSource, ModuleName, TFS,
                                    BuiltModuleFiles)) {
    BuiltModuleFiles.recordRequiredSourceForLookup(ModuleName, RequiredSource);
    return llvm::Error::success();
  }

  std::string ModuleUnitFileName =
      MDB.getSourceForModuleName(ModuleName, RequiredSource);
  /// It is possible that we're meeting third party modules (modules whose
  /// source are not in the project. e.g, the std module may be a third-party
  /// module for most project) or something wrong with the implementation of
  /// ProjectModules.
  /// FIXME: How should we treat third party modules here? If we want to ignore
  /// third party modules, we should return true instead of false here.
  /// Currently we simply bail out.
  if (ModuleUnitFileName.empty())
    return llvm::createStringError(
        llvm::formatv("Don't get the module unit for module {0}", ModuleName));

  /// Try to get prebuilt module files from the compilation database first. This
  /// helps to avoid building the module files that are already built by the
  /// compiler.
  getPrebuiltModuleFile(ModuleName, ModuleUnitFileName, TFS, BuiltModuleFiles);

  // Get Required modules in topological order.
  auto ReqModules = getAllRequiredModules(RequiredSource, MDB, ModuleName);
  for (const auto &ReqModule : ReqModules) {
    llvm::StringRef ReqModuleName = ReqModule.Name;
    if (BuiltModuleFiles.isModuleUnitBuilt(ReqModuleName)) {
      if (auto Cmd = getCDB().getCompileCommand(ReqModule.RequiredSource)) {
        auto ValidationCI =
            buildCompilerInvocationForCommand(*Cmd, TFS.view(std::nullopt));
        if (ValidationCI) {
          if (auto It =
                  ValidationCI->getHeaderSearchOpts().PrebuiltModuleFiles.find(
                      ReqModuleName);
              It != ValidationCI->getHeaderSearchOpts().PrebuiltModuleFiles.end() &&
              IsModuleFileFullyUpToDate(It->second, BuiltModuleFiles,
                                        TFS.view(std::nullopt),
                                        ValidationCI.get())) {
            if (!BuiltModuleFiles.matchesBuiltModuleConfiguration(
                    ReqModuleName, ValidationCI.get(),
                    /*ModuleSourceIdentity=*/"",
                    /*CompileCommandFingerprint=*/""))
              return llvm::createStringError(llvm::formatv(
                  "Conflicting module lookup for module {0}", ReqModuleName));
            BuiltModuleFiles.recordRequiredSourceForLookup(
                ReqModuleName, ReqModule.RequiredSource);
            continue;
          }
        }
      }
    }

    if (getExplicitPrebuiltModuleFile(ReqModule.RequiredSource, ReqModuleName,
                                      TFS, BuiltModuleFiles)) {
      BuiltModuleFiles.recordRequiredSourceForLookup(ReqModuleName,
                                                     ReqModule.RequiredSource);
      continue;
    }

    std::string ReqFileName =
        MDB.getSourceForModuleName(ReqModuleName, ReqModule.RequiredSource);
    if (ReqFileName.empty())
      return llvm::createStringError(llvm::formatv(
          "Don't get the module unit for module {0}", ReqModuleName));
    auto ReqConfig = getModuleLookupConfiguration(ReqFileName, getCDB(), TFS);

    if (auto Cached = Cache.getModule(ReqModuleName, ReqFileName)) {
      if (ReqConfig.CI &&
          Cached->getModuleSourceIdentity() == ReqConfig.SourceIdentity &&
          Cached->getCompileCommandFingerprint() ==
              ReqConfig.CommandFingerprint &&
          IsModuleFileFullyUpToDate(Cached->getModuleFilePath(),
                                    BuiltModuleFiles, TFS.view(std::nullopt),
                                    ReqConfig.CI.get())) {
        log("Reusing module {0} from {1}", ReqModuleName,
            Cached->getModuleFilePath());
        BuiltModuleFiles.recordRequiredSourceForLookup(ReqModuleName,
                                                       ReqModule.RequiredSource);
        BuiltModuleFiles.addModuleFile(std::move(Cached));
        continue;
      }
      Cache.remove(ReqModuleName, ReqFileName);
    }

    if (BuiltModuleFiles.isModuleUnitBuilt(ReqModuleName)) {
      if (!BuiltModuleFiles.matchesBuiltModuleConfiguration(
              ReqModuleName, ReqConfig.CI.get(), ReqConfig.SourceIdentity,
              ReqConfig.CommandFingerprint))
        return llvm::createStringError(llvm::formatv(
            "Conflicting module lookup for module {0}", ReqModuleName));
      BuiltModuleFiles.recordRequiredSourceForLookup(ReqModuleName,
                                                     ReqModule.RequiredSource);
      continue;
    }

    llvm::Expected<std::shared_ptr<BuiltModuleFile>> MF =
        buildModuleFile(ReqModuleName, ReqFileName, ReqModule.RequiredSource,
                        ReqConfig.SourceIdentity, ReqConfig.CommandFingerprint,
                        getCDB(), TFS,
                        BuiltModuleFiles);
    if (llvm::Error Err = MF.takeError())
      return Err;

    log("Built module {0} to {1}", ReqModuleName, (*MF)->getModuleFilePath());
    Cache.add(ReqModuleName, ReqFileName, *MF);
    BuiltModuleFiles.addModuleFile(std::move(*MF));
  }

  return llvm::Error::success();
}

std::unique_ptr<PrerequisiteModules>
ModulesBuilder::buildPrerequisiteModulesFor(PathRef File,
                                            const ThreadsafeFS &TFS) {
  if (auto Cmd = Impl->getCDB().getCompileCommand(File)) {
    auto ValidationCI =
        buildCompilerInvocationForCommand(*Cmd, TFS.view(std::nullopt));
    if (ValidationCI)
      if (auto Cached = Impl->getPrerequisiteModulesCache().getReusable(
              File, *ValidationCI, TFS.view(std::nullopt)))
        return std::make_unique<ReusablePrerequisiteModules>(*Cached);
  }

  std::unique_ptr<ProjectModules> MDB = Impl->getCDB().getProjectModules(File);
  if (!MDB) {
    elog("Failed to get Project Modules information for {0}", File);
    return std::make_unique<FailedPrerequisiteModules>();
  }
  CachingProjectModules CachedMDB(std::move(MDB),
                                  Impl->getProjectModulesCache());

  std::vector<std::string> RequiredModuleNames =
      CachedMDB.getRequiredModules(File);
  auto RequiredModules = std::make_unique<ReusablePrerequisiteModules>(
      File, Impl->getCDB(), RequiredModuleNames);
  if (RequiredModuleNames.empty())
    return RequiredModules;
  for (llvm::StringRef RequiredModuleName : RequiredModuleNames) {
    // Return early if there is any error.
    if (llvm::Error Err = Impl->getOrBuildModuleFile(
            File, RequiredModuleName, TFS, CachedMDB, *RequiredModules.get())) {
      elog("Failed to build module {0}; due to {1}", RequiredModuleName,
           toString(std::move(Err)));
      return std::make_unique<FailedPrerequisiteModules>();
    }
  }

  Impl->getPrerequisiteModulesCache().add(
      File,
      std::make_shared<ReusablePrerequisiteModules>(*RequiredModules));
  return std::move(RequiredModules);
}

ModulesBuilder::ModulesBuilder(const GlobalCompilationDatabase &CDB) {
  Impl = std::make_unique<ModulesBuilderImpl>(CDB);
}

ModulesBuilder::~ModulesBuilder() {}

} // namespace clangd
} // namespace clang
