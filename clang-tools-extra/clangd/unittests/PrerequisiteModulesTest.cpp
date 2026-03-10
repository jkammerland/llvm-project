//===--------------- PrerequisiteModulesTests.cpp -------------------*- C++
//-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

/// FIXME: Skip testing on windows temporarily due to the different escaping
/// code mode.
#ifndef _WIN32

#include "Annotations.h"
#include "CodeComplete.h"
#include "Compiler.h"
#include "ModulesBuilder.h"
#include "ScanningProjectModules.h"
#include "TestTU.h"
#include "support/ThreadsafeFS.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace clang::clangd {
namespace {

class GlobalScanningCounterProjectModules : public ProjectModules {
public:
  GlobalScanningCounterProjectModules(
      std::unique_ptr<ProjectModules> Underlying, std::atomic<unsigned> &Count)
      : Underlying(std::move(Underlying)), Count(Count) {}

  std::vector<std::string> getRequiredModules(PathRef File) override {
    return Underlying->getRequiredModules(File);
  }

  std::string getModuleNameForSource(PathRef File) override {
    return Underlying->getModuleNameForSource(File);
  }

  void setCommandMangler(CommandMangler Mangler) override {
    Underlying->setCommandMangler(std::move(Mangler));
  }

  std::string getSourceForModuleName(llvm::StringRef ModuleName,
                                     PathRef RequiredSrcFile) override {
    Count++;
    return Underlying->getSourceForModuleName(ModuleName, RequiredSrcFile);
  }

private:
  std::unique_ptr<ProjectModules> Underlying;
  std::atomic<unsigned> &Count;
};

class MockDirectoryCompilationDatabase : public MockCompilationDatabase {
public:
  MockDirectoryCompilationDatabase(StringRef TestDir, const ThreadsafeFS &TFS)
      : MockCompilationDatabase(TestDir),
        MockedCDBPtr(std::make_shared<MockClangCompilationDatabase>(*this)),
        TFS(TFS), GlobalScanningCount(0) {
    this->ExtraClangFlags.push_back("-std=c++20");
    this->ExtraClangFlags.push_back("-c");
  }

  void addFile(llvm::StringRef Path, llvm::StringRef Contents);

  std::unique_ptr<ProjectModules> getProjectModules(PathRef) const override {
    return std::make_unique<GlobalScanningCounterProjectModules>(
        scanningProjectModules(MockedCDBPtr, TFS), GlobalScanningCount);
  }

  unsigned getGlobalScanningCount() const { return GlobalScanningCount; }

private:
  class MockClangCompilationDatabase : public tooling::CompilationDatabase {
  public:
    MockClangCompilationDatabase(MockDirectoryCompilationDatabase &MCDB)
        : MCDB(MCDB) {}

    std::vector<tooling::CompileCommand>
    getCompileCommands(StringRef FilePath) const override {
      std::optional<tooling::CompileCommand> Cmd =
          MCDB.getCompileCommand(FilePath);
      EXPECT_TRUE(Cmd);
      return {*Cmd};
    }

    std::vector<std::string> getAllFiles() const override { return Files; }

    void AddFile(StringRef File) { Files.push_back(File.str()); }

  private:
    MockDirectoryCompilationDatabase &MCDB;
    std::vector<std::string> Files;
  };

  std::shared_ptr<MockClangCompilationDatabase> MockedCDBPtr;
  const ThreadsafeFS &TFS;

  mutable std::atomic<unsigned> GlobalScanningCount;
};

class FilenameSensitiveProjectModules : public ProjectModules {
public:
  explicit FilenameSensitiveProjectModules(PathRef TestDir)
      : TestDir(TestDir.str()) {}

  std::vector<std::string> getRequiredModules(PathRef File) override {
    llvm::StringRef FileName = llvm::sys::path::filename(File);
    if (FileName == "UseA.cpp" || FileName == "UseB.cpp")
      return {"M"};
    return {};
  }

  std::string getModuleNameForSource(PathRef File) override {
    llvm::StringRef FileName = llvm::sys::path::filename(File);
    if (FileName == "M-A.cppm" || FileName == "M-B.cppm")
      return "M";
    return {};
  }

  std::string getSourceForModuleName(llvm::StringRef ModuleName,
                                     PathRef RequiredSrcFile) override {
    if (ModuleName != "M")
      return {};

    llvm::StringRef RequiredFileName =
        llvm::sys::path::filename(RequiredSrcFile);
    if (RequiredFileName == "UseA.cpp" || RequiredFileName == "M-A.cppm")
      return getPathFor("M-A.cppm");
    if (RequiredFileName == "UseB.cpp" || RequiredFileName == "M-B.cppm")
      return getPathFor("M-B.cppm");
    return {};
  }

private:
  std::string getPathFor(llvm::StringRef RelativePath) const {
    llvm::SmallString<128> FullPath(TestDir);
    llvm::sys::path::append(FullPath, RelativePath);
    return FullPath.str().str();
  }

  std::string TestDir;
};

class FilenameSensitiveMockDirectoryCompilationDatabase
    : public MockDirectoryCompilationDatabase {
public:
  FilenameSensitiveMockDirectoryCompilationDatabase(StringRef TestDir,
                                                    const ThreadsafeFS &TFS)
      : MockDirectoryCompilationDatabase(TestDir, TFS), TestDir(TestDir) {}

  std::unique_ptr<ProjectModules> getProjectModules(PathRef) const override {
    return std::make_unique<FilenameSensitiveProjectModules>(TestDir);
  }

private:
  std::string TestDir;
};

class SourceSwitchingProjectModules : public ProjectModules {
public:
  SourceSwitchingProjectModules(llvm::StringRef PrimarySource,
                                llvm::StringRef AlternateSource,
                                const bool &UseAlternateSource)
      : PrimarySource(PrimarySource.str()),
        AlternateSource(AlternateSource.str()),
        UseAlternateSource(UseAlternateSource) {}

  std::vector<std::string> getRequiredModules(PathRef File) override {
    if (llvm::sys::path::filename(File) == "Use.cpp")
      return {"M"};
    return {};
  }

  std::string getModuleNameForSource(PathRef File) override {
    if (File == PrimarySource)
      return UseAlternateSource ? "NotM" : "M";
    if (File == AlternateSource)
      return UseAlternateSource ? "M" : "NotM";
    return "";
  }

  std::string getSourceForModuleName(llvm::StringRef ModuleName,
                                     PathRef) override {
    if (ModuleName != "M")
      return "";
    return UseAlternateSource ? AlternateSource : PrimarySource;
  }

private:
  std::string PrimarySource;
  std::string AlternateSource;
  const bool &UseAlternateSource;
};

class SourceSwitchingCompilationDatabase
    : public MockDirectoryCompilationDatabase {
public:
  SourceSwitchingCompilationDatabase(StringRef TestDir, const ThreadsafeFS &TFS)
      : MockDirectoryCompilationDatabase(TestDir, TFS) {}

  void setUseAlternateSource(bool Value) { UseAlternateSource = Value; }

  std::unique_ptr<ProjectModules> getProjectModules(PathRef) const override {
    llvm::SmallString<256> PrimarySource(Directory);
    llvm::sys::path::append(PrimarySource, "M-primary.cppm");
    llvm::SmallString<256> AlternateSource(Directory);
    llvm::sys::path::append(AlternateSource, "M-alternate.cppm");
    return std::make_unique<SourceSwitchingProjectModules>(
        PrimarySource.str(), AlternateSource.str(), UseAlternateSource);
  }

private:
  mutable bool UseAlternateSource = false;
};

class ModuleUnitFlagCompilationDatabase
    : public MockDirectoryCompilationDatabase {
public:
  ModuleUnitFlagCompilationDatabase(StringRef TestDir, const ThreadsafeFS &TFS)
      : MockDirectoryCompilationDatabase(TestDir, TFS) {}

  void setModuleFlagValue(int Value) { ModuleFlagValue = Value; }

  std::optional<tooling::CompileCommand>
  getCompileCommand(PathRef File) const override {
    auto Cmd = MockDirectoryCompilationDatabase::getCompileCommand(File);
    if (!Cmd)
      return std::nullopt;
    if (llvm::sys::path::filename(File) == "M.cppm")
      Cmd->CommandLine.push_back("-DMODULE_FLAG=" +
                                 std::to_string(ModuleFlagValue));
    return Cmd;
  }

private:
  int ModuleFlagValue = 1;
};

// Add files to the working testing directory and the compilation database.
void MockDirectoryCompilationDatabase::addFile(llvm::StringRef Path,
                                               llvm::StringRef Contents) {
  ASSERT_FALSE(llvm::sys::path::is_absolute(Path));

  SmallString<256> AbsPath(Directory);
  llvm::sys::path::append(AbsPath, Path);

  ASSERT_FALSE(
      llvm::sys::fs::create_directories(llvm::sys::path::parent_path(AbsPath)));

  std::error_code EC;
  llvm::raw_fd_ostream OS(AbsPath, EC);
  ASSERT_FALSE(EC);
  OS << Contents;

  MockedCDBPtr->AddFile(Path);
}

class PrerequisiteModulesTests : public ::testing::Test {
protected:
  void SetUp() override {
    ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("modules-test", TestDir));
  }

  void TearDown() override {
    ASSERT_FALSE(llvm::sys::fs::remove_directories(TestDir));
  }

public:
  // Get the absolute path for file specified by Path under testing working
  // directory.
  std::string getFullPath(llvm::StringRef Path) {
    SmallString<128> Result(TestDir);
    llvm::sys::path::append(Result, Path);
    EXPECT_TRUE(llvm::sys::fs::exists(Result.str()));
    return Result.str().str();
  }

  ParseInputs getInputs(llvm::StringRef FileName,
                        const GlobalCompilationDatabase &CDB) {
    std::string FullPathName = getFullPath(FileName);

    ParseInputs Inputs;
    std::optional<tooling::CompileCommand> Cmd =
        CDB.getCompileCommand(FullPathName);
    EXPECT_TRUE(Cmd);
    Inputs.CompileCommand = std::move(*Cmd);
    Inputs.TFS = &FS;

    if (auto Contents = FS.view(TestDir)->getBufferForFile(FullPathName))
      Inputs.Contents = Contents->get()->getBuffer().str();

    return Inputs;
  }

  SmallString<256> TestDir;
  // FIXME: It will be better to use the MockFS if the scanning process and
  // build module process doesn't depend on reading real IO.
  RealThreadsafeFS FS;

  DiagnosticConsumer DiagConsumer;
};

TEST_F(PrerequisiteModulesTests, NonModularTest) {
  MockDirectoryCompilationDatabase CDB(TestDir, FS);

  CDB.addFile("foo.h", R"cpp(
inline void foo() {}
  )cpp");

  CDB.addFile("NonModular.cpp", R"cpp(
#include "foo.h"
void use() {
  foo();
}
  )cpp");

  ModulesBuilder Builder(CDB);

  // NonModular.cpp is not related to modules. So nothing should be built.
  auto NonModularInfo =
      Builder.buildPrerequisiteModulesFor(getFullPath("NonModular.cpp"), FS);
  EXPECT_TRUE(NonModularInfo);

  HeaderSearchOptions HSOpts;
  NonModularInfo->adjustHeaderSearchOptions(HSOpts);
  EXPECT_TRUE(HSOpts.PrebuiltModuleFiles.empty());

  auto Invocation =
      buildCompilerInvocation(getInputs("NonModular.cpp", CDB), DiagConsumer);
  EXPECT_TRUE(NonModularInfo->canReuse(*Invocation, FS.view(TestDir)));
}

TEST_F(PrerequisiteModulesTests, ModuleWithoutDepTest) {
  MockDirectoryCompilationDatabase CDB(TestDir, FS);

  CDB.addFile("foo.h", R"cpp(
inline void foo() {}
  )cpp");

  CDB.addFile("M.cppm", R"cpp(
module;
#include "foo.h"
export module M;
  )cpp");

  ModulesBuilder Builder(CDB);

  auto MInfo = Builder.buildPrerequisiteModulesFor(getFullPath("M.cppm"), FS);
  EXPECT_TRUE(MInfo);

  // Nothing should be built since M doesn't dependent on anything.
  HeaderSearchOptions HSOpts;
  MInfo->adjustHeaderSearchOptions(HSOpts);
  EXPECT_TRUE(HSOpts.PrebuiltModuleFiles.empty());

  auto Invocation =
      buildCompilerInvocation(getInputs("M.cppm", CDB), DiagConsumer);
  EXPECT_TRUE(MInfo->canReuse(*Invocation, FS.view(TestDir)));
}

TEST_F(PrerequisiteModulesTests, ModuleWithArgumentPatch) {
  MockDirectoryCompilationDatabase CDB(TestDir, FS);

  CDB.ExtraClangFlags.push_back("-invalid-unknown-flag");

  CDB.addFile("Dep.cppm", R"cpp(
export module Dep;
  )cpp");

  CDB.addFile("M.cppm", R"cpp(
export module M;
import Dep;
  )cpp");

  // An invalid flag will break the module compilation and the
  // getRequiredModules would return an empty array
  auto ProjectModules = CDB.getProjectModules(getFullPath("M.cppm"));
  EXPECT_TRUE(
      ProjectModules->getRequiredModules(getFullPath("M.cppm")).empty());

  // Set the mangler to filter out the invalid flag
  ProjectModules->setCommandMangler([](tooling::CompileCommand &Command,
                                       PathRef) {
    auto const It = llvm::find(Command.CommandLine, "-invalid-unknown-flag");
    Command.CommandLine.erase(It);
  });

  // And now it returns a non-empty list of required modules since the
  // compilation succeeded
  EXPECT_FALSE(
      ProjectModules->getRequiredModules(getFullPath("M.cppm")).empty());
}

TEST_F(PrerequisiteModulesTests, ModuleWithDepTest) {
  MockDirectoryCompilationDatabase CDB(TestDir, FS);

  CDB.addFile("foo.h", R"cpp(
inline void foo() {}
  )cpp");

  CDB.addFile("M.cppm", R"cpp(
module;
#include "foo.h"
export module M;
  )cpp");

  CDB.addFile("N.cppm", R"cpp(
export module N;
import :Part;
import M;
  )cpp");

  CDB.addFile("N-part.cppm", R"cpp(
// Different module name with filename intentionally.
export module N:Part;
  )cpp");

  ModulesBuilder Builder(CDB);

  auto NInfo = Builder.buildPrerequisiteModulesFor(getFullPath("N.cppm"), FS);
  EXPECT_TRUE(NInfo);

  ParseInputs NInput = getInputs("N.cppm", CDB);
  std::unique_ptr<CompilerInvocation> Invocation =
      buildCompilerInvocation(NInput, DiagConsumer);
  // Test that `PrerequisiteModules::canReuse` works basically.
  EXPECT_TRUE(NInfo->canReuse(*Invocation, FS.view(TestDir)));

  {
    // Check that
    // `PrerequisiteModules::adjustHeaderSearchOptions(HeaderSearchOptions&)`
    // can appending HeaderSearchOptions correctly.
    HeaderSearchOptions HSOpts;
    NInfo->adjustHeaderSearchOptions(HSOpts);

    EXPECT_TRUE(HSOpts.PrebuiltModuleFiles.count("M"));
    EXPECT_TRUE(HSOpts.PrebuiltModuleFiles.count("N:Part"));
  }

  {
    // Check that
    // `PrerequisiteModules::adjustHeaderSearchOptions(HeaderSearchOptions&)`
    // can replace HeaderSearchOptions correctly.
    HeaderSearchOptions HSOpts;
    HSOpts.PrebuiltModuleFiles["M"] = "incorrect_path";
    HSOpts.PrebuiltModuleFiles["N:Part"] = "incorrect_path";
    NInfo->adjustHeaderSearchOptions(HSOpts);

    EXPECT_TRUE(StringRef(HSOpts.PrebuiltModuleFiles["M"]).ends_with(".pcm"));
    EXPECT_TRUE(
        StringRef(HSOpts.PrebuiltModuleFiles["N:Part"]).ends_with(".pcm"));
  }
}

TEST_F(PrerequisiteModulesTests, ReusabilityTest) {
  MockDirectoryCompilationDatabase CDB(TestDir, FS);

  CDB.addFile("foo.h", R"cpp(
inline void foo() {}
  )cpp");

  CDB.addFile("M.cppm", R"cpp(
module;
#include "foo.h"
export module M;
  )cpp");

  CDB.addFile("N.cppm", R"cpp(
export module N;
import :Part;
import M;
  )cpp");

  CDB.addFile("N-part.cppm", R"cpp(
// Different module name with filename intentionally.
export module N:Part;
  )cpp");

  ModulesBuilder Builder(CDB);

  auto NInfo = Builder.buildPrerequisiteModulesFor(getFullPath("N.cppm"), FS);
  EXPECT_TRUE(NInfo);
  EXPECT_TRUE(NInfo);

  ParseInputs NInput = getInputs("N.cppm", CDB);
  std::unique_ptr<CompilerInvocation> Invocation =
      buildCompilerInvocation(NInput, DiagConsumer);
  EXPECT_TRUE(NInfo->canReuse(*Invocation, FS.view(TestDir)));

  // Test that we can still reuse the NInfo after we touch a unrelated file.
  {
    CDB.addFile("L.cppm", R"cpp(
module;
#include "foo.h"
export module L;
export int ll = 43;
  )cpp");
    EXPECT_TRUE(NInfo->canReuse(*Invocation, FS.view(TestDir)));

    CDB.addFile("bar.h", R"cpp(
inline void bar() {}
inline void bar(int) {}
  )cpp");
    EXPECT_TRUE(NInfo->canReuse(*Invocation, FS.view(TestDir)));
  }

  // Test that we can't reuse the NInfo after we touch a related file.
  {
    CDB.addFile("M.cppm", R"cpp(
module;
#include "foo.h"
export module M;
export int mm = 44;
  )cpp");
    EXPECT_FALSE(NInfo->canReuse(*Invocation, FS.view(TestDir)));

    NInfo = Builder.buildPrerequisiteModulesFor(getFullPath("N.cppm"), FS);
    EXPECT_TRUE(NInfo->canReuse(*Invocation, FS.view(TestDir)));

    CDB.addFile("foo.h", R"cpp(
inline void foo() {}
inline void foo(int) {}
  )cpp");
    EXPECT_FALSE(NInfo->canReuse(*Invocation, FS.view(TestDir)));

    NInfo = Builder.buildPrerequisiteModulesFor(getFullPath("N.cppm"), FS);
    EXPECT_TRUE(NInfo->canReuse(*Invocation, FS.view(TestDir)));
  }

  CDB.addFile("N-part.cppm", R"cpp(
export module N:Part;
// Intentioned to make it uncompilable.
export int NPart = 4LIdjwldijaw
  )cpp");
  EXPECT_FALSE(NInfo->canReuse(*Invocation, FS.view(TestDir)));
  NInfo = Builder.buildPrerequisiteModulesFor(getFullPath("N.cppm"), FS);
  EXPECT_TRUE(NInfo);
  EXPECT_FALSE(NInfo->canReuse(*Invocation, FS.view(TestDir)));

  CDB.addFile("N-part.cppm", R"cpp(
export module N:Part;
export int NPart = 43;
  )cpp");
  EXPECT_TRUE(NInfo);
  EXPECT_FALSE(NInfo->canReuse(*Invocation, FS.view(TestDir)));
  NInfo = Builder.buildPrerequisiteModulesFor(getFullPath("N.cppm"), FS);
  EXPECT_TRUE(NInfo);
  EXPECT_TRUE(NInfo->canReuse(*Invocation, FS.view(TestDir)));

  // Test that if we changed the modification time of the file, the module files
  // info is still reusable if its content doesn't change.
  CDB.addFile("N-part.cppm", R"cpp(
export module N:Part;
export int NPart = 43;
  )cpp");
  EXPECT_TRUE(NInfo->canReuse(*Invocation, FS.view(TestDir)));

  CDB.addFile("N.cppm", R"cpp(
export module N;
import :Part;
import M;

export int nn = 43;
  )cpp");
  // NInfo should be reusable after we change its content.
  EXPECT_TRUE(NInfo->canReuse(*Invocation, FS.view(TestDir)));
}

TEST_F(PrerequisiteModulesTests, AddingImportInvalidatesReuse) {
  MockDirectoryCompilationDatabase CDB(TestDir, FS);

  CDB.addFile("M.cppm", R"cpp(
export module M;
export int MValue = 43;
  )cpp");

  CDB.addFile("Use.cpp", R"cpp(
int use() {
  return 0;
}
  )cpp");

  ModulesBuilder Builder(CDB);
  auto UseInfo =
      Builder.buildPrerequisiteModulesFor(getFullPath("Use.cpp"), FS);
  EXPECT_TRUE(UseInfo);

  ParseInputs UseInput = getInputs("Use.cpp", CDB);
  std::unique_ptr<CompilerInvocation> Invocation =
      buildCompilerInvocation(UseInput, DiagConsumer);
  EXPECT_TRUE(UseInfo->canReuse(*Invocation, FS.view(TestDir)));

  CDB.addFile("Use.cpp", R"cpp(
import M;
int use() {
  return MValue;
}
  )cpp");
  UseInput = getInputs("Use.cpp", CDB);
  Invocation = buildCompilerInvocation(UseInput, DiagConsumer);
  EXPECT_FALSE(UseInfo->canReuse(*Invocation, FS.view(TestDir)));

  UseInfo = Builder.buildPrerequisiteModulesFor(getFullPath("Use.cpp"), FS);
  EXPECT_TRUE(UseInfo);
  EXPECT_TRUE(UseInfo->canReuse(*Invocation, FS.view(TestDir)));

  HeaderSearchOptions HSOpts;
  UseInfo->adjustHeaderSearchOptions(HSOpts);
  EXPECT_TRUE(HSOpts.PrebuiltModuleFiles.count("M"));
}

TEST_F(PrerequisiteModulesTests, ModulesPreambleCompatibility) {
  MockDirectoryCompilationDatabase CDB(TestDir, FS);

  CDB.addFile("A.cppm", R"cpp(
export module A;
export void printA();
  )cpp");
  CDB.addFile("Header.hpp", R"cpp(
#define HAS_PREAMBLE 1
  )cpp");
  CDB.addFile("Use.cpp", R"cpp(
#include "Header.hpp"
import A;
void foo() {}
  )cpp");

  ModulesBuilder Builder(CDB);

  ParseInputs Use = getInputs("Use.cpp", CDB);
  Use.ModulesManager = &Builder;

  std::unique_ptr<CompilerInvocation> CI =
      buildCompilerInvocation(Use, DiagConsumer);
  ASSERT_TRUE(CI);

  auto Preamble =
      buildPreamble(getFullPath("Use.cpp"), *CI, Use, /*InMemory=*/true,
                    /*Callback=*/nullptr);
  ASSERT_TRUE(Preamble);
  EXPECT_EQ(Preamble->Preamble.getBounds().Size, 0u);
  EXPECT_TRUE(isPreambleCompatible(*Preamble, Use, getFullPath("Use.cpp"),
                                   *CI));
}

TEST_F(PrerequisiteModulesTests, IncludeDrivenImportInMainAST) {
  MockDirectoryCompilationDatabase CDB(TestDir, FS);

  CDB.addFile("A.cppm", R"cpp(
export module A;
export void printA();
  )cpp");
  CDB.addFile("Header.hpp", R"cpp(
import A;
  )cpp");
  CDB.addFile("Use.cpp", R"cpp(
#include "Header.hpp"
void foo() {
  printA();
}
  )cpp");

  ModulesBuilder Builder(CDB);

  ParseInputs Use = getInputs("Use.cpp", CDB);
  Use.ModulesManager = &Builder;

  std::unique_ptr<CompilerInvocation> CI =
      buildCompilerInvocation(Use, DiagConsumer);
  ASSERT_TRUE(CI);

  auto Preamble =
      buildPreamble(getFullPath("Use.cpp"), *CI, Use, /*InMemory=*/true,
                    /*Callback=*/nullptr);
  ASSERT_TRUE(Preamble);
  EXPECT_EQ(Preamble->Preamble.getBounds().Size, 0u);

  auto AST = ParsedAST::build(getFullPath("Use.cpp"), Use, std::move(CI), {},
                              Preamble);
  ASSERT_TRUE(AST);

  const NamedDecl &D = findDecl(*AST, "printA");
  EXPECT_TRUE(D.isFromASTFile());
}
// An End-to-End test for modules.
TEST_F(PrerequisiteModulesTests, ParsedASTTest) {
  MockDirectoryCompilationDatabase CDB(TestDir, FS);

  CDB.addFile("A.cppm", R"cpp(
export module A;
export void printA();
  )cpp");

  CDB.addFile("Use.cpp", R"cpp(
import A;
)cpp");

  ModulesBuilder Builder(CDB);

  ParseInputs Use = getInputs("Use.cpp", CDB);
  Use.ModulesManager = &Builder;

  std::unique_ptr<CompilerInvocation> CI =
      buildCompilerInvocation(Use, DiagConsumer);
  EXPECT_TRUE(CI);

  auto Preamble =
      buildPreamble(getFullPath("Use.cpp"), *CI, Use, /*InMemory=*/true,
                    /*Callback=*/nullptr);
  EXPECT_TRUE(Preamble);
  EXPECT_TRUE(Preamble->RequiredModules);

  auto AST = ParsedAST::build(getFullPath("Use.cpp"), Use, std::move(CI), {},
                              Preamble);
  EXPECT_TRUE(AST);

  const NamedDecl &D = findDecl(*AST, "printA");
  EXPECT_TRUE(D.isFromASTFile());
}

// Regression test for #100924.
TEST_F(PrerequisiteModulesTests, NoFalseODRViolationInMultipleGMFs) {
  MockDirectoryCompilationDatabase CDB(TestDir, FS);

  {
    SmallString<256> SharedHeader(TestDir);
    llvm::sys::path::append(SharedHeader, "shared.h");
    std::error_code EC;
    llvm::raw_fd_ostream OS(SharedHeader, EC);
    ASSERT_FALSE(EC);
    OS << R"cpp(
template <class T>
concept HasFoo = requires {
  T::foo;
};

template <class T>
struct Category {
  static constexpr int message = T::foo;
};
  )cpp";
  }

  CDB.addFile("b.cppm", R"cpp(
module;
#include "shared.h"
export module b;

export template <class T>
concept C = requires() {
  Category<T>::message;
};
  )cpp");

  CDB.addFile("c.cppm", R"cpp(
module;
#include "shared.h"

export module c;
import b;

class B {
public:
  static constexpr int foo = 1;
};

export template <class T>
  requires C<T>
class D {};

D<B> S;
  )cpp");

  ModulesBuilder Builder(CDB);

  ParseInputs Input = getInputs("c.cppm", CDB);
  Input.ModulesManager = &Builder;

  std::unique_ptr<CompilerInvocation> CI =
      buildCompilerInvocation(Input, DiagConsumer);
  EXPECT_TRUE(CI);

  auto Preamble =
      buildPreamble(getFullPath("c.cppm"), *CI, Input, /*InMemory=*/true,
                    /*Callback=*/nullptr);
  EXPECT_TRUE(Preamble);
  EXPECT_TRUE(Preamble->RequiredModules);
  HeaderSearchOptions HSOpts;
  Preamble->RequiredModules->adjustHeaderSearchOptions(HSOpts);
  EXPECT_TRUE(HSOpts.PrebuiltModuleFiles.count("b"));

  auto AST = ParsedAST::build(getFullPath("c.cppm"), Input, std::move(CI), {},
                              Preamble);
  EXPECT_TRUE(AST);

  for (const auto &Diag : AST->getDiagnostics())
    EXPECT_NE(Diag.Name, "module_odr_violation_missing_decl");
}

// An end to end test for code complete in modules
TEST_F(PrerequisiteModulesTests, CodeCompleteTest) {
  MockDirectoryCompilationDatabase CDB(TestDir, FS);

  CDB.addFile("A.cppm", R"cpp(
export module A;
export void printA();
  )cpp");

  llvm::StringLiteral UserContents = R"cpp(
import A;
void func() {
  print^
}
)cpp";

  CDB.addFile("Use.cpp", UserContents);
  Annotations Test(UserContents);

  ModulesBuilder Builder(CDB);

  ParseInputs Use = getInputs("Use.cpp", CDB);
  Use.ModulesManager = &Builder;

  std::unique_ptr<CompilerInvocation> CI =
      buildCompilerInvocation(Use, DiagConsumer);
  EXPECT_TRUE(CI);

  auto Preamble =
      buildPreamble(getFullPath("Use.cpp"), *CI, Use, /*InMemory=*/true,
                    /*Callback=*/nullptr);
  EXPECT_TRUE(Preamble);
  EXPECT_TRUE(Preamble->RequiredModules);

  auto Result = codeComplete(getFullPath("Use.cpp"), Test.point(),
                             Preamble.get(), Use, {});
  EXPECT_FALSE(Result.Completions.empty());
  EXPECT_EQ(Result.Completions[0].Name, "printA");
}

TEST_F(PrerequisiteModulesTests, CodeCompleteModuleDocsTest) {
  MockDirectoryCompilationDatabase CDB(TestDir, FS);

  CDB.addFile("A.cppm", R"cpp(
export module A;
/// Print A value.
export void printA();
  )cpp");

  llvm::StringLiteral UserContents = R"cpp(
import A;
void func() {
  print^
}
)cpp";

  CDB.addFile("Use.cpp", UserContents);
  Annotations Test(UserContents);

  ModulesBuilder Builder(CDB);

  ParseInputs Use = getInputs("Use.cpp", CDB);
  Use.ModulesManager = &Builder;

  std::unique_ptr<CompilerInvocation> CI =
      buildCompilerInvocation(Use, DiagConsumer);
  EXPECT_TRUE(CI);

  auto Preamble =
      buildPreamble(getFullPath("Use.cpp"), *CI, Use, /*InMemory=*/true,
                    /*Callback=*/nullptr);
  EXPECT_TRUE(Preamble);
  EXPECT_TRUE(Preamble->RequiredModules);

  auto Result = codeComplete(getFullPath("Use.cpp"), Test.point(),
                             Preamble.get(), Use, {});
  EXPECT_FALSE(Result.Completions.empty());

  const CodeCompletion *PrintA = nullptr;
  for (const auto &Completion : Result.Completions) {
    if (Completion.Name == "printA") {
      PrintA = &Completion;
      break;
    }
  }
  ASSERT_TRUE(PrintA);
  ASSERT_TRUE(PrintA->Documentation);
  EXPECT_THAT(PrintA->Documentation->asPlainText(),
              testing::HasSubstr("Print A value."));
}

TEST_F(PrerequisiteModulesTests, CodeCompleteModuleDocsFromPrebuiltNamedModule) {
  MockDirectoryCompilationDatabase CDB(TestDir, FS);

  CDB.addFile("A.cppm", R"cpp(
export module A;
/// Print A value.
export void printA();
  )cpp");

  llvm::StringLiteral UserContents = R"cpp(
import A;
void func() {
  print^
}
)cpp";
  CDB.addFile("Use.cpp", UserContents);
  Annotations Test(UserContents);

  ModulesBuilder Builder(CDB);
  auto BuiltModules =
      Builder.buildPrerequisiteModulesFor(getFullPath("Use.cpp"), FS);
  ASSERT_TRUE(BuiltModules);

  HeaderSearchOptions HSOpts(TestDir);
  BuiltModules->adjustHeaderSearchOptions(HSOpts);
  auto It = HSOpts.PrebuiltModuleFiles.find("A");
  ASSERT_NE(It, HSOpts.PrebuiltModuleFiles.end());
  ASSERT_TRUE(llvm::sys::fs::exists(It->second));

  CDB.ExtraClangFlags.push_back("-fmodule-file=A=" + It->second);
  ParseInputs Use = getInputs("Use.cpp", CDB);

  std::unique_ptr<CompilerInvocation> CI =
      buildCompilerInvocation(Use, DiagConsumer);
  ASSERT_TRUE(CI);

  auto Preamble =
      buildPreamble(getFullPath("Use.cpp"), *CI, Use, /*InMemory=*/true,
                    /*Callback=*/nullptr);
  ASSERT_TRUE(Preamble);

  auto Result = codeComplete(getFullPath("Use.cpp"), Test.point(),
                             Preamble.get(), Use, {});
  ASSERT_FALSE(Result.Completions.empty());

  const CodeCompletion *PrintA = nullptr;
  for (const auto &Completion : Result.Completions) {
    if (Completion.Name == "printA") {
      PrintA = &Completion;
      break;
    }
  }
  ASSERT_TRUE(PrintA);
  ASSERT_TRUE(PrintA->Documentation);
  EXPECT_THAT(PrintA->Documentation->asPlainText(),
              testing::HasSubstr("Print A value."));
}

TEST_F(PrerequisiteModulesTests, ModuleInternalMacroNotCompletedInImporter) {
  MockDirectoryCompilationDatabase CDB(TestDir, FS);

  CDB.addFile("A.cppm", R"cpp(
export module A;
#define A_INTERNAL_MACRO 1
export void A_INTERNAL_FN();
  )cpp");

  llvm::StringLiteral UserContents = R"cpp(
import A;
void func() {
  A_INT^
}
)cpp";

  CDB.addFile("Use.cpp", UserContents);
  Annotations Test(UserContents);

  ModulesBuilder Builder(CDB);

  ParseInputs Use = getInputs("Use.cpp", CDB);
  Use.ModulesManager = &Builder;

  std::unique_ptr<CompilerInvocation> CI =
      buildCompilerInvocation(Use, DiagConsumer);
  EXPECT_TRUE(CI);

  auto Preamble =
      buildPreamble(getFullPath("Use.cpp"), *CI, Use, /*InMemory=*/true,
                    /*Callback=*/nullptr);
  EXPECT_TRUE(Preamble);
  EXPECT_TRUE(Preamble->RequiredModules);

  auto Result = codeComplete(getFullPath("Use.cpp"), Test.point(),
                             Preamble.get(), Use, {});
  EXPECT_THAT(Result.Completions,
              testing::Contains(testing::Field(&CodeCompletion::Name,
                                               "A_INTERNAL_FN")));
  EXPECT_THAT(Result.Completions,
              testing::Not(testing::Contains(testing::Field(
                  &CodeCompletion::Name, "A_INTERNAL_MACRO"))));
}
TEST_F(PrerequisiteModulesTests, SignatureHelpTest) {
  MockDirectoryCompilationDatabase CDB(TestDir, FS);

  CDB.addFile("A.cppm", R"cpp(
export module A;
export void printA(int a);
  )cpp");

  llvm::StringLiteral UserContents = R"cpp(
import A;
void func() {
  printA(^);
}
)cpp";

  CDB.addFile("Use.cpp", UserContents);
  Annotations Test(UserContents);

  ModulesBuilder Builder(CDB);

  ParseInputs Use = getInputs("Use.cpp", CDB);
  Use.ModulesManager = &Builder;

  std::unique_ptr<CompilerInvocation> CI =
      buildCompilerInvocation(Use, DiagConsumer);
  EXPECT_TRUE(CI);

  auto Preamble =
      buildPreamble(getFullPath("Use.cpp"), *CI, Use, /*InMemory=*/true,
                    /*Callback=*/nullptr);
  EXPECT_TRUE(Preamble);
  EXPECT_TRUE(Preamble->RequiredModules);

  auto Result = signatureHelp(getFullPath("Use.cpp"), Test.point(), *Preamble,
                              Use, MarkupKind::PlainText);
  EXPECT_FALSE(Result.signatures.empty());
  EXPECT_EQ(Result.signatures[0].label, "printA(int a) -> void");
  EXPECT_EQ(Result.signatures[0].parameters[0].labelString, "int a");
}

TEST_F(PrerequisiteModulesTests, ReusablePrerequisiteModulesTest) {
  MockDirectoryCompilationDatabase CDB(TestDir, FS);

  CDB.addFile("M.cppm", R"cpp(
export module M;
export int M = 43;
  )cpp");
  CDB.addFile("A.cppm", R"cpp(
export module A;
import M;
export int A = 43 + M;
  )cpp");
  CDB.addFile("B.cppm", R"cpp(
export module B;
import M;
export int B = 44 + M;
  )cpp");

  ModulesBuilder Builder(CDB);

  auto AInfo = Builder.buildPrerequisiteModulesFor(getFullPath("A.cppm"), FS);
  EXPECT_TRUE(AInfo);
  auto BInfo = Builder.buildPrerequisiteModulesFor(getFullPath("B.cppm"), FS);
  EXPECT_TRUE(BInfo);
  HeaderSearchOptions HSOptsA(TestDir);
  HeaderSearchOptions HSOptsB(TestDir);
  AInfo->adjustHeaderSearchOptions(HSOptsA);
  BInfo->adjustHeaderSearchOptions(HSOptsB);

  EXPECT_FALSE(HSOptsA.PrebuiltModuleFiles.empty());
  EXPECT_FALSE(HSOptsB.PrebuiltModuleFiles.empty());

  // Check that we're reusing the module files.
  EXPECT_EQ(HSOptsA.PrebuiltModuleFiles, HSOptsB.PrebuiltModuleFiles);

  // Update M.cppm to check if the modules builder can update correctly.
  CDB.addFile("M.cppm", R"cpp(
export module M;
export constexpr int M = 43;
  )cpp");

  ParseInputs AUse = getInputs("A.cppm", CDB);
  AUse.ModulesManager = &Builder;
  std::unique_ptr<CompilerInvocation> AInvocation =
      buildCompilerInvocation(AUse, DiagConsumer);
  EXPECT_FALSE(AInfo->canReuse(*AInvocation, FS.view(TestDir)));

  ParseInputs BUse = getInputs("B.cppm", CDB);
  AUse.ModulesManager = &Builder;
  std::unique_ptr<CompilerInvocation> BInvocation =
      buildCompilerInvocation(BUse, DiagConsumer);
  EXPECT_FALSE(BInfo->canReuse(*BInvocation, FS.view(TestDir)));

  auto NewAInfo =
      Builder.buildPrerequisiteModulesFor(getFullPath("A.cppm"), FS);
  auto NewBInfo =
      Builder.buildPrerequisiteModulesFor(getFullPath("B.cppm"), FS);
  EXPECT_TRUE(NewAInfo);
  EXPECT_TRUE(NewBInfo);
  HeaderSearchOptions NewHSOptsA(TestDir);
  HeaderSearchOptions NewHSOptsB(TestDir);
  NewAInfo->adjustHeaderSearchOptions(NewHSOptsA);
  NewBInfo->adjustHeaderSearchOptions(NewHSOptsB);

  EXPECT_FALSE(NewHSOptsA.PrebuiltModuleFiles.empty());
  EXPECT_FALSE(NewHSOptsB.PrebuiltModuleFiles.empty());

  EXPECT_EQ(NewHSOptsA.PrebuiltModuleFiles, NewHSOptsB.PrebuiltModuleFiles);
  // Check that we didn't reuse the old and stale module files.
  EXPECT_NE(NewHSOptsA.PrebuiltModuleFiles, HSOptsA.PrebuiltModuleFiles);
}

TEST_F(PrerequisiteModulesTests, ScanningCacheTest) {
  MockDirectoryCompilationDatabase CDB(TestDir, FS);

  CDB.addFile("M.cppm", R"cpp(
export module M;
  )cpp");
  CDB.addFile("A.cppm", R"cpp(
export module A;
import M;
  )cpp");
  CDB.addFile("B.cppm", R"cpp(
export module B;
import M;
  )cpp");

  ModulesBuilder Builder(CDB);

  Builder.buildPrerequisiteModulesFor(getFullPath("A.cppm"), FS);
  Builder.buildPrerequisiteModulesFor(getFullPath("B.cppm"), FS);
  // Lookups are keyed by module name and required source file.
  EXPECT_EQ(CDB.getGlobalScanningCount(), 2u);
}

TEST_F(PrerequisiteModulesTests, RequiredSourceSensitiveModuleCaches) {
  FilenameSensitiveMockDirectoryCompilationDatabase CDB(TestDir, FS);

  CDB.addFile("M-A.cppm", R"cpp(
export module M;
export int fromA = 43;
  )cpp");
  CDB.addFile("M-B.cppm", R"cpp(
export module M;
export int fromB = 44;
  )cpp");

  CDB.addFile("UseA.cpp", R"cpp(
import M;
int useA = fromA;
  )cpp");
  CDB.addFile("UseB.cpp", R"cpp(
import M;
int useB = fromB;
  )cpp");

  ModulesBuilder Builder(CDB);
  auto AInfo = Builder.buildPrerequisiteModulesFor(getFullPath("UseA.cpp"), FS);
  auto BInfo = Builder.buildPrerequisiteModulesFor(getFullPath("UseB.cpp"), FS);

  EXPECT_TRUE(AInfo);
  EXPECT_TRUE(BInfo);

  HeaderSearchOptions AHSOpts(TestDir);
  HeaderSearchOptions BHSOpts(TestDir);
  AInfo->adjustHeaderSearchOptions(AHSOpts);
  BInfo->adjustHeaderSearchOptions(BHSOpts);

  ASSERT_TRUE(AHSOpts.PrebuiltModuleFiles.count("M"));
  ASSERT_TRUE(BHSOpts.PrebuiltModuleFiles.count("M"));

  // UseA and UseB resolve module M to different module-unit sources.
  EXPECT_NE(AHSOpts.PrebuiltModuleFiles["M"], BHSOpts.PrebuiltModuleFiles["M"]);
}

TEST_F(PrerequisiteModulesTests, PrebuiltModuleFileTest) {
  MockDirectoryCompilationDatabase CDB(TestDir, FS);

  CDB.addFile("M.cppm", R"cpp(
export module M;
  )cpp");

  CDB.addFile("U.cpp", R"cpp(
import M;
  )cpp");

  // Use ModulesBuilder to produce the prebuilt module file.
  ModulesBuilder Builder(CDB);
  auto ModuleInfo =
      Builder.buildPrerequisiteModulesFor(getFullPath("U.cpp"), FS);
  HeaderSearchOptions HS(TestDir);
  ModuleInfo->adjustHeaderSearchOptions(HS);

  CDB.ExtraClangFlags.push_back("-fmodule-file=M=" +
                                HS.PrebuiltModuleFiles["M"]);
  ModulesBuilder Builder2(CDB);
  auto ModuleInfo2 =
      Builder2.buildPrerequisiteModulesFor(getFullPath("U.cpp"), FS);
  HeaderSearchOptions HS2(TestDir);
  ModuleInfo2->adjustHeaderSearchOptions(HS2);

  EXPECT_EQ(HS.PrebuiltModuleFiles, HS2.PrebuiltModuleFiles);
}

TEST_F(PrerequisiteModulesTests, CacheRejectsSourceRemapWithSameModuleName) {
  SourceSwitchingCompilationDatabase CDB(TestDir, FS);

  CDB.addFile("M-primary.cppm", R"cpp(
export module M;
export constexpr int MValue = 1;
  )cpp");
  CDB.addFile("M-alternate.cppm", R"cpp(
export module M;
export constexpr int MValue = 2;
  )cpp");
  CDB.addFile("Use.cpp", R"cpp(
import M;
int UseM = MValue;
  )cpp");

  ModulesBuilder Builder(CDB);
  auto FirstInfo =
      Builder.buildPrerequisiteModulesFor(getFullPath("Use.cpp"), FS);
  ASSERT_TRUE(FirstInfo);
  HeaderSearchOptions FirstHS(TestDir);
  FirstInfo->adjustHeaderSearchOptions(FirstHS);
  ASSERT_TRUE(FirstHS.PrebuiltModuleFiles.count("M"));
  std::string FirstModulePath = FirstHS.PrebuiltModuleFiles["M"];

  CDB.setUseAlternateSource(true);
  auto SecondInfo =
      Builder.buildPrerequisiteModulesFor(getFullPath("Use.cpp"), FS);
  ASSERT_TRUE(SecondInfo);
  HeaderSearchOptions SecondHS(TestDir);
  SecondInfo->adjustHeaderSearchOptions(SecondHS);
  ASSERT_TRUE(SecondHS.PrebuiltModuleFiles.count("M"));

  EXPECT_NE(FirstModulePath, SecondHS.PrebuiltModuleFiles["M"]);
}

TEST_F(PrerequisiteModulesTests, CacheRejectsModuleUnitCompileFlagsMismatch) {
  ModuleUnitFlagCompilationDatabase CDB(TestDir, FS);

  CDB.addFile("M.cppm", R"cpp(
export module M;
export constexpr int MValue = MODULE_FLAG;
  )cpp");
  CDB.addFile("U.cpp", R"cpp(
import M;
int UseM = MValue;
  )cpp");

  ModulesBuilder Builder(CDB);
  auto FirstInfo =
      Builder.buildPrerequisiteModulesFor(getFullPath("U.cpp"), FS);
  ASSERT_TRUE(FirstInfo);
  HeaderSearchOptions FirstHS(TestDir);
  FirstInfo->adjustHeaderSearchOptions(FirstHS);
  ASSERT_TRUE(FirstHS.PrebuiltModuleFiles.count("M"));
  std::string FirstModulePath = FirstHS.PrebuiltModuleFiles["M"];

  CDB.setModuleFlagValue(2);
  auto SecondInfo =
      Builder.buildPrerequisiteModulesFor(getFullPath("U.cpp"), FS);
  ASSERT_TRUE(SecondInfo);
  HeaderSearchOptions SecondHS(TestDir);
  SecondInfo->adjustHeaderSearchOptions(SecondHS);
  ASSERT_TRUE(SecondHS.PrebuiltModuleFiles.count("M"));

  EXPECT_NE(FirstModulePath, SecondHS.PrebuiltModuleFiles["M"]);
}

TEST_F(PrerequisiteModulesTests, ReuseRejectsCompileCommandMismatch) {
  MockDirectoryCompilationDatabase CDB(TestDir, FS);

  CDB.ExtraClangFlags.push_back("-DMODULE_FLAG=1");
  CDB.addFile("M.cppm", R"cpp(
export module M;
export constexpr int MValue = MODULE_FLAG;
  )cpp");
  CDB.addFile("U.cpp", R"cpp(
import M;
int UseM = MValue;
  )cpp");

  ModulesBuilder Builder(CDB);
  auto ModuleInfo =
      Builder.buildPrerequisiteModulesFor(getFullPath("U.cpp"), FS);
  ASSERT_TRUE(ModuleInfo);

  auto Invocation =
      buildCompilerInvocation(getInputs("U.cpp", CDB), DiagConsumer);
  ASSERT_TRUE(Invocation);
  EXPECT_TRUE(ModuleInfo->canReuse(*Invocation, FS.view(TestDir)));

  CDB.ExtraClangFlags.pop_back();
  CDB.ExtraClangFlags.push_back("-DMODULE_FLAG=2");
  auto NewInvocation =
      buildCompilerInvocation(getInputs("U.cpp", CDB), DiagConsumer);
  ASSERT_TRUE(NewInvocation);
  EXPECT_FALSE(ModuleInfo->canReuse(*NewInvocation, FS.view(TestDir)));
}

TEST_F(PrerequisiteModulesTests, ReuseRejectsContextHashMismatch) {
  MockDirectoryCompilationDatabase CDB(TestDir, FS);

  CDB.addFile("M.cppm", R"cpp(
export module M;
export constexpr int MValue = 1;
  )cpp");
  CDB.addFile("U.cpp", R"cpp(
import M;
int UseM = MValue;
  )cpp");

  ModulesBuilder Builder(CDB);
  auto ModuleInfo =
      Builder.buildPrerequisiteModulesFor(getFullPath("U.cpp"), FS);
  ASSERT_TRUE(ModuleInfo);

  HeaderSearchOptions HS(TestDir);
  ModuleInfo->adjustHeaderSearchOptions(HS);
  ASSERT_TRUE(HS.PrebuiltModuleFiles.count("M"));

  std::error_code EC;
  llvm::raw_fd_ostream OS(HS.PrebuiltModuleFiles["M"] + ".ctxhash", EC);
  ASSERT_FALSE(EC);
  OS << "manually-mismatched-context-hash";
  OS.close();

  auto Invocation =
      buildCompilerInvocation(getInputs("U.cpp", CDB), DiagConsumer);
  ASSERT_TRUE(Invocation);
  EXPECT_FALSE(ModuleInfo->canReuse(*Invocation, FS.view(TestDir)));
}

TEST_F(PrerequisiteModulesTests, CacheRejectsCompileCommandMismatch) {
  MockDirectoryCompilationDatabase CDB(TestDir, FS);

  CDB.ExtraClangFlags.push_back("-DMODULE_FLAG=1");
  CDB.addFile("M.cppm", R"cpp(
export module M;
export constexpr int MValue = MODULE_FLAG;
  )cpp");
  CDB.addFile("U.cpp", R"cpp(
import M;
int UseM = MValue;
  )cpp");

  ModulesBuilder Builder(CDB);
  auto FirstInfo =
      Builder.buildPrerequisiteModulesFor(getFullPath("U.cpp"), FS);
  ASSERT_TRUE(FirstInfo);
  HeaderSearchOptions FirstHS(TestDir);
  FirstInfo->adjustHeaderSearchOptions(FirstHS);
  ASSERT_TRUE(FirstHS.PrebuiltModuleFiles.count("M"));
  std::string FirstModulePath = FirstHS.PrebuiltModuleFiles["M"];

  CDB.ExtraClangFlags.pop_back();
  CDB.ExtraClangFlags.push_back("-DMODULE_FLAG=2");
  auto SecondInfo =
      Builder.buildPrerequisiteModulesFor(getFullPath("U.cpp"), FS);
  ASSERT_TRUE(SecondInfo);
  HeaderSearchOptions SecondHS(TestDir);
  SecondInfo->adjustHeaderSearchOptions(SecondHS);
  ASSERT_TRUE(SecondHS.PrebuiltModuleFiles.count("M"));

  EXPECT_NE(FirstModulePath, SecondHS.PrebuiltModuleFiles["M"]);
}

TEST_F(PrerequisiteModulesTests, PrebuiltRejectsCompileCommandMismatch) {
  MockDirectoryCompilationDatabase CDB(TestDir, FS);

  CDB.ExtraClangFlags.push_back("-DMODULE_FLAG=1");
  CDB.addFile("M.cppm", R"cpp(
export module M;
export constexpr int MValue = MODULE_FLAG;
  )cpp");
  CDB.addFile("U.cpp", R"cpp(
import M;
int UseM = MValue;
  )cpp");

  ModulesBuilder Builder(CDB);
  auto ModuleInfo =
      Builder.buildPrerequisiteModulesFor(getFullPath("U.cpp"), FS);
  ASSERT_TRUE(ModuleInfo);
  HeaderSearchOptions HS(TestDir);
  ModuleInfo->adjustHeaderSearchOptions(HS);
  ASSERT_TRUE(HS.PrebuiltModuleFiles.count("M"));
  std::string OldPrebuiltPath = HS.PrebuiltModuleFiles["M"];

  CDB.ExtraClangFlags.pop_back();
  CDB.ExtraClangFlags.push_back("-DMODULE_FLAG=2");
  CDB.ExtraClangFlags.push_back("-fmodule-file=M=" + OldPrebuiltPath);
  ModulesBuilder Builder2(CDB);
  auto ModuleInfo2 =
      Builder2.buildPrerequisiteModulesFor(getFullPath("U.cpp"), FS);
  ASSERT_TRUE(ModuleInfo2);
  HeaderSearchOptions HS2(TestDir);
  ModuleInfo2->adjustHeaderSearchOptions(HS2);
  ASSERT_TRUE(HS2.PrebuiltModuleFiles.count("M"));

  EXPECT_NE(OldPrebuiltPath, HS2.PrebuiltModuleFiles["M"]);
}

} // namespace
} // namespace clang::clangd

#endif
