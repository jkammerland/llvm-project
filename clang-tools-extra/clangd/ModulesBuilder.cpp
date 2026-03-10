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
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include <optional>
#include <queue>

namespace clang {
namespace clangd {

namespace {

llvm::cl::opt<bool> DebugModulesBuilder(
    "debug-modules-builder",
    llvm::cl::desc("Don't remove clangd's built module files for debugging. "
                   "Remember to remove them later after debugging."),
    llvm::cl::init(false));

// Create a path to store module files. Generally it should be:
//
//   {TEMP_DIRS}/clangd/module_files/{hashed-file-name}-%%-%%-%%-%%-%%-%%/.
//
// {TEMP_DIRS} is the temporary directory for the system, e.g., "/var/tmp"
// or "C:/TEMP".
//
// '%%' means random value to make the generated path unique.
//
// \param MainFile is used to get the root of the project from global
// compilation database.
//
// TODO: Move these module fils out of the temporary directory if the module
// files are persistent.
llvm::SmallString<256> getUniqueModuleFilesPath(PathRef MainFile) {
  llvm::SmallString<128> HashedPrefix = llvm::sys::path::filename(MainFile);
  // There might be multiple files with the same name in a project. So appending
  // the hash value of the full path to make sure they won't conflict.
  HashedPrefix += std::to_string(llvm::hash_value(MainFile));

  llvm::SmallString<256> ResultPattern;

  llvm::sys::path::system_temp_directory(/*erasedOnReboot=*/true,
                                         ResultPattern);

  llvm::sys::path::append(ResultPattern, "clangd");
  llvm::sys::path::append(ResultPattern, "module_files");

  llvm::sys::path::append(ResultPattern, HashedPrefix);

  ResultPattern.append("-%%-%%-%%-%%-%%-%%");

  llvm::SmallString<256> Result;
  llvm::sys::fs::createUniquePath(ResultPattern, Result,
                                  /*MakeAbsolute=*/false);

  llvm::sys::fs::create_directories(Result);
  return Result;
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

  llvm::hash_code Hash =
      llvm::hash_combine(CompileCommand.Directory, CompileCommand.Filename);
  for (const auto &Arg : CommandLine)
    Hash = llvm::hash_combine(Hash, Arg);
  return std::to_string(static_cast<uint64_t>(Hash));
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

void applyModuleBuildInvocationSettings(CompilerInvocation &CI) {
  // In clang's driver, we suppress ODR checks in GMF while building modules.
  // Keep reuse validation aligned with that mode.
  CI.getLangOpts().SkipODRCheckInGMF = true;

  // Hash the contents of input files and preserve comments so reused BMIs match
  // the way clangd originally built them.
  CI.getHeaderSearchOpts().ValidateASTInputFilesContent = true;
  CI.getPreprocessorOpts().WriteCommentListToPCH = true;
  CI.getPreprocessorOpts().WriteCommentListToNamedModules = true;
}

bool IsModuleFileUpToDate(PathRef ModuleFilePath,
                          const PrerequisiteModules &RequisiteModules,
                          llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS,
                          const CompilerInvocation *CI,
                          bool CheckStoredContextHash = true);

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
                  StringRef RequiredSourceForLookup, CtorTag)
      : ModuleFile(ModuleName, ModuleFilePath, ModuleSourceIdentity,
                   CompileCommandFingerprint, RequiredSourceForLookup) {}

  static std::shared_ptr<BuiltModuleFile> make(StringRef ModuleName,
                                               PathRef ModuleFilePath,
                                               StringRef ModuleSourceIdentity,
                                               StringRef CompileCommandHash,
                                               StringRef RequiredSourceForLookup) {
    return std::make_shared<BuiltModuleFile>(ModuleName, ModuleFilePath,
                                             ModuleSourceIdentity,
                                             CompileCommandHash,
                                             RequiredSourceForLookup,
                                             CtorTag{});
  }

  virtual ~BuiltModuleFile() {
    if (!ModuleFilePath.empty() && !DebugModulesBuilder) {
      llvm::sys::fs::remove(ModuleFilePath);
      llvm::sys::fs::remove(getModuleContextHashFilePath(ModuleFilePath));
    }
  }
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

      if (auto It = RequiredSourcesForLookup.find(MF->getModuleName());
          It != RequiredSourcesForLookup.end()) {
        if (!llvm::all_of(It->second, HasSameSourceBackedConfiguration))
          return false;
        continue;
      }

      PathRef RequiredSource = MF->getRequiredSourceForLookup().empty()
                                   ? MainFile
                                   : MF->getRequiredSourceForLookup();
      if (!HasSameSourceBackedConfiguration(RequiredSource))
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

    CompilerInvocation ImportValidationCI(ImporterCI);
    ImportValidationCI.getPreprocessorOpts() =
        ValidationCI->getPreprocessorOpts();
    if (!IsModuleFileUpToDate(MF.getModuleFilePath(), *this, VFS,
                              &ImportValidationCI,
                              /*CheckStoredContextHash=*/true))
      return false;

    // Revalidate the BMI against the prerequisite prefix that existed when it
    // was originally built, not the whole final reusable set.
    StaticPrerequisiteModules BuiltBeforeCurrent(
        llvm::ArrayRef(RequiredModules).take_front(Index));
    return IsModuleFileUpToDate(MF.getModuleFilePath(), BuiltBeforeCurrent, VFS,
                                ValidationCI.get());
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
                          bool CheckStoredContextHash) {
  if (CI)
    if (auto StoredContextHash = readModuleContextHashFile(ModuleFilePath, VFS);
        CheckStoredContextHash &&
            StoredContextHash && *StoredContextHash != CI->computeContextHash())
      return false;

  HeaderSearchOptions HSOpts;
  LangOptions LangOpts;
  PreprocessorOptions PPOpts;
  if (CI) {
    HSOpts = CI->getHeaderSearchOpts();
    LangOpts = CI->getLangOpts();
    PPOpts = CI->getPreprocessorOpts();
  }
  RequisiteModules.adjustHeaderSearchOptions(HSOpts);
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
                   PCHOperations.getRawReader(), CodeGenOpts, {});

  // We don't need any listener here. By default it will use a validator
  // listener.
  Reader.setListener(nullptr);

  const unsigned ValidationCaps =
      ASTReader::ARR_OutOfDate | ASTReader::ARR_Missing |
      ASTReader::ARR_ConfigurationMismatch |
      ASTReader::ARR_TreatModuleWithErrorsAsOutOfDate;
  if (Reader.ReadAST(ModuleFilePath, serialization::MK_MainFile,
                     SourceLocation(), ValidationCaps) != ASTReader::Success)
    return false;

  bool UpToDate = true;
  Reader.getModuleManager().visit([&](serialization::ModuleFile &MF) -> bool {
    Reader.visitInputFiles(
        MF, /*IncludeSystem=*/false, /*Complain=*/false,
        [&](const serialization::InputFile &IF, bool isSystem) {
          if (!IF.getFile() || IF.isOutOfDate())
            UpToDate = false;
        });
    return !UpToDate;
  });
  return UpToDate;
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

  llvm::SmallString<256> ModuleFilesPrefix =
      getUniqueModuleFilesPath(ModuleUnitFileName);

  Cmd->Output = getModuleFilePath(ModuleName, ModuleFilesPrefix);

  ParseInputs Inputs;
  Inputs.TFS = &TFS;
  Inputs.CompileCommand = std::move(*Cmd);

  IgnoreDiagnostics IgnoreDiags;
  auto CI = buildCompilerInvocation(Inputs, IgnoreDiags);
  if (!CI)
    return llvm::createStringError("Failed to build compiler invocation");

  auto FS = Inputs.TFS->view(Inputs.CompileCommand.Directory);
  auto Buf = FS->getBufferForFile(Inputs.CompileCommand.Filename);
  if (!Buf)
    return llvm::createStringError("Failed to create buffer");

  applyModuleBuildInvocationSettings(*CI);

  BuiltModuleFiles.adjustHeaderSearchOptions(CI->getHeaderSearchOpts());
  const std::string ModuleContextHash = CI->computeContextHash();

  CI->getFrontendOpts().OutputFile = Inputs.CompileCommand.Output;
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

  return BuiltModuleFile::make(ModuleName, Inputs.CompileCommand.Output,
                               ModuleSourceIdentity,
                               CompileCommandFingerprint,
                               RequiredSourceForLookup);
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

  if (RequiredModules.empty())
    return true;

  for (size_t I = 0; I < RequiredModules.size(); ++I) {
    const auto &MF = *RequiredModules[I];

    if (MF.getModuleSourceIdentity().empty()) {
      if (!IsModuleFileUpToDate(MF.getModuleFilePath(), *this, VFS, &CI,
                                /*CheckStoredContextHash=*/false))
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
  ModuleFileCache(const GlobalCompilationDatabase &CDB) : CDB(CDB) {}
  const GlobalCompilationDatabase &getCDB() const { return CDB; }

  std::shared_ptr<const ModuleFile> getModule(StringRef ModuleName,
                                              PathRef ModuleUnitSourcePath);

  void add(StringRef ModuleName, PathRef ModuleUnitSourcePath,
           std::shared_ptr<const ModuleFile> ModuleFile) {
    std::lock_guard<std::mutex> Lock(ModuleFilesMutex);

    ModuleFiles[ModuleName][maybeCaseFoldPath(ModuleUnitSourcePath)] =
        ModuleFile;
  }

  void remove(StringRef ModuleName, PathRef ModuleUnitSourcePath);

private:
  const GlobalCompilationDatabase &CDB;

  llvm::StringMap<llvm::StringMap<std::weak_ptr<const ModuleFile>>> ModuleFiles;
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

  if (auto Res = SourceIt->second.lock())
    return Res;

  It->second.erase(SourceIt);
  if (It->second.empty())
    ModuleFiles.erase(It);
  return nullptr;
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
    std::string CachedResult =
        Cache.getSourceForModuleName(ModuleName, RequiredSrcFile);

    // Verify Cached Result by seeing if the source declaring the same module
    // as we query.
    if (!CachedResult.empty()) {
      std::string ModuleNameOfCachedSource =
          MDB->getModuleNameForSource(CachedResult);
      if (ModuleNameOfCachedSource == ModuleName)
        return CachedResult;

      // Cached Result is invalid. Clear it.
      Cache.eraseEntry(ModuleName, RequiredSrcFile);
    }

    auto Result = MDB->getSourceForModuleName(ModuleName, RequiredSrcFile);
    Cache.addEntry(ModuleName, RequiredSrcFile, Result);

    return Result;
  }

private:
  std::unique_ptr<ProjectModules> MDB;
  ModuleNameToSourceCache &Cache;
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
  ModulesBuilderImpl(const GlobalCompilationDatabase &CDB) : Cache(CDB) {}

  ModuleNameToSourceCache &getProjectModulesCache() {
    return ProjectModulesCache;
  }
  const GlobalCompilationDatabase &getCDB() const { return Cache.getCDB(); }

  llvm::Error
  getOrBuildModuleFile(PathRef RequiredSource, StringRef ModuleName,
                       const ThreadsafeFS &TFS, CachingProjectModules &MDB,
                       ReusablePrerequisiteModules &BuiltModuleFiles);

private:
  /// Try to get prebuilt module files from the compilation database.
  void getPrebuiltModuleFile(StringRef ModuleName, PathRef ModuleUnitFileName,
                             const ThreadsafeFS &TFS,
                             ReusablePrerequisiteModules &BuiltModuleFiles);

  ModuleFileCache Cache;
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

    if (IsModuleFileUpToDate(ModuleFilePath, BuiltModuleFiles,
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

llvm::Error ModulesBuilder::ModulesBuilderImpl::getOrBuildModuleFile(
    PathRef RequiredSource, StringRef ModuleName, const ThreadsafeFS &TFS,
    CachingProjectModules &MDB, ReusablePrerequisiteModules &BuiltModuleFiles) {
  if (BuiltModuleFiles.isModuleUnitBuilt(ModuleName)) {
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
      BuiltModuleFiles.recordRequiredSourceForLookup(ReqModuleName,
                                                     ReqModule.RequiredSource);
      continue;
    }

    std::string ReqFileName =
        MDB.getSourceForModuleName(ReqModuleName, ReqModule.RequiredSource);
    if (ReqFileName.empty())
      return llvm::createStringError(llvm::formatv(
          "Don't get the module unit for module {0}", ReqModuleName));
    std::unique_ptr<CompilerInvocation> ReqCI;
    std::string ReqSourceIdentity;
    std::string ReqCommandFingerprint;
    if (auto ReqCmd = getCDB().getCompileCommand(ReqFileName)) {
      ReqSourceIdentity = getResolvedModuleSourceIdentity(
          ReqFileName, ReqCmd->Directory, TFS.view(std::nullopt));
      ReqCommandFingerprint = getCompileCommandFingerprint(*ReqCmd);
      ParseInputs Inputs;
      Inputs.TFS = &TFS;
      Inputs.CompileCommand = std::move(*ReqCmd);
      IgnoreDiagnostics IgnoreDiags;
      ReqCI = buildCompilerInvocation(Inputs, IgnoreDiags);
    } else {
      ReqSourceIdentity =
          getResolvedModuleSourceIdentity(ReqFileName, "", TFS.view(std::nullopt));
    }

    if (auto Cached = Cache.getModule(ReqModuleName, ReqFileName)) {
      if (ReqCI && Cached->getModuleSourceIdentity() == ReqSourceIdentity &&
          Cached->getCompileCommandFingerprint() == ReqCommandFingerprint &&
          IsModuleFileUpToDate(Cached->getModuleFilePath(), BuiltModuleFiles,
                               TFS.view(std::nullopt), ReqCI.get())) {
        log("Reusing module {0} from {1}", ReqModuleName,
            Cached->getModuleFilePath());
        BuiltModuleFiles.recordRequiredSourceForLookup(ReqModuleName,
                                                       ReqModule.RequiredSource);
        BuiltModuleFiles.addModuleFile(std::move(Cached));
        continue;
      }
      Cache.remove(ReqModuleName, ReqFileName);
    }

    llvm::Expected<std::shared_ptr<BuiltModuleFile>> MF =
        buildModuleFile(ReqModuleName, ReqFileName, ReqModule.RequiredSource,
                        ReqSourceIdentity, ReqCommandFingerprint, getCDB(), TFS,
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

  return std::move(RequiredModules);
}

ModulesBuilder::ModulesBuilder(const GlobalCompilationDatabase &CDB) {
  Impl = std::make_unique<ModulesBuilderImpl>(CDB);
}

ModulesBuilder::~ModulesBuilder() {}

} // namespace clangd
} // namespace clang
