//===--- hsh.cpp - hsh tool driver ----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/WithColor.h"

#include "clang/Basic/FileManager.h"
#include "clang/Basic/Version.h"
#include "clang/Config/config.h"
#include "clang/Driver/Driver.h"
#include "clang/Hsh/HshGenerator.h"
#include "clang/Tooling/Tooling.h"

using namespace llvm;
using namespace clang;
using namespace clang::driver;

namespace llvm::cl {
class driver_style_bool_parser : public parser<bool> {
public:
  using parser::parser;
  void printOptionInfo(const Option &O, size_t GlobalWidth) const {
    outs() << "  -" << O.ArgStr << " ";
    Option::printHelpStr(O.HelpStr, GlobalWidth, getOptionWidth(O));
  }
};
template <class ImplClass>
class driver_style_string_parser : public parser<std::string> {
public:
  using parser::parser;
  void printOptionInfo(const Option &O, size_t GlobalWidth) const {
    outs() << "  -" << O.ArgStr << " <" << getValueName() << ">";
    for (size_t I = 0; I < ImplClass::Pad; ++I)
      outs() << ' ';
    Option::printHelpStr(O.HelpStr, GlobalWidth, getOptionWidth(O));
  }
};
class dir_parser : public driver_style_string_parser<dir_parser> {
public:
  static constexpr size_t Pad = 0;
  using driver_style_string_parser::driver_style_string_parser;
  StringRef getValueName() const override { return "dir"; }
};
class def_parser : public driver_style_string_parser<def_parser> {
public:
  static constexpr size_t Pad = 0;
  using driver_style_string_parser::driver_style_string_parser;
  StringRef getValueName() const override { return "macro>=<value"; }
};
class file_parser : public driver_style_string_parser<file_parser> {
public:
  static constexpr size_t Pad = 1;
  using driver_style_string_parser::driver_style_string_parser;
  StringRef getValueName() const override { return "file"; }
};
class value_parser : public driver_style_string_parser<value_parser> {
public:
  static constexpr size_t Pad = 1;
  using driver_style_string_parser::driver_style_string_parser;
  StringRef getValueName() const override { return "value"; }
};
} // namespace llvm::cl

int main(int argc, const char **argv) {
  static cl::opt<bool> Verbose(
      "v", cl::desc("Show commands to run and use verbose output"),
      cl::cat(llvm::cl::GeneralCategory));

  static cl::list<std::string, bool, cl::dir_parser> IncludeDirs(
      "I", cl::ZeroOrMore, cl::Prefix,
      cl::desc("Add directory to include search path"),
      cl::cat(llvm::cl::GeneralCategory));

  static cl::list<std::string, bool, cl::def_parser> CompileDefs(
      "D", cl::ZeroOrMore, cl::Prefix,
      cl::desc("Define <macro> to <value> (or 1 if <value> omitted)"),
      cl::cat(llvm::cl::GeneralCategory));

  static cl::opt<bool, false, cl::driver_style_bool_parser> MD(
      "MD", cl::desc("Write a depfile containing user and system headers"),
      cl::cat(llvm::cl::GeneralCategory));

  static cl::opt<std::string, false, cl::file_parser> MF(
      "MF", cl::desc("Write depfile output from -MD to <file>"),
      cl::cat(llvm::cl::GeneralCategory));

  static cl::opt<std::string, false, cl::value_parser> MT(
      "MT", cl::desc("Specify name of main file output in depfile"),
      cl::cat(llvm::cl::GeneralCategory));

  static cl::opt<std::string> Input(cl::Positional, cl::desc("<input>"),
                                    cl::Required,
                                    cl::cat(llvm::cl::GeneralCategory));

  static cl::opt<std::string> Output(cl::Positional, cl::desc("<output>"),
                                     cl::Required,
                                     cl::cat(llvm::cl::GeneralCategory));

  static cl::OptionCategory HshCategory("Hsh Generator Options");

  struct TargetOption {
    hshgen::HshTarget Target;
    cl::opt<bool> Opt;
    TargetOption(hshgen::HshTarget Target, StringRef Desc)
        : Target(Target), Opt(hshgen::HshTargetToString(Target), cl::desc(Desc),
                              cl::cat(HshCategory)) {}
    operator bool() const { return Opt.operator bool(); }
  };
  static TargetOption HshTargets[] = {
      {hshgen::HT_GLSL, "GLSL Source Target"},
      {hshgen::HT_HLSL, "HLSL Source Target"},
      {hshgen::HT_DXBC, "DXBC Binary Target (requires d3dcompiler.dll)"},
      {hshgen::HT_DXIL, "DXIL Binary Target (requires dxcompiler.dll)"},
      {hshgen::HT_VULKAN_SPIRV,
       "Vulkan SPIR-V Binary Target (requires dxcompiler.dll)"},
      {hshgen::HT_METAL, "Metal Source Target"},
      {hshgen::HT_METAL_BIN_MAC, "Metal Binary macOS Target (requires Xcode)"},
      {hshgen::HT_METAL_BIN_IOS, "Metal Binary iOS Target (requires Xcode)"},
      {hshgen::HT_METAL_BIN_TVOS, "Metal Binary tvOS Target (requires Xcode)"},
  };

  if (!cl::ParseCommandLineOptions(argc, argv, "Hsh Codegen Tool"))
    return 1;

  std::vector<std::string> args = {argv[0],
#ifdef __linux__
                                   "--gcc-toolchain=/usr",
#endif
                                   "-c",
                                   "-std=c++17",
                                   "-D__hsh__=1",
                                   "-Wno-expansion-to-defined",
                                   "-Wno-nullability-completeness",
                                   "-Wno-unused-value",
                                   "-Wno-undefined-inline"};
  if (Verbose)
    args.emplace_back("-v");
  if (WithColor(llvm::errs()).colorsEnabled())
    args.emplace_back("-fcolor-diagnostics");
  for (const auto &Dir : IncludeDirs) {
    args.emplace_back("-I");
    args.push_back(Dir);
  }
  for (const auto &Def : CompileDefs) {
    constexpr llvm::StringLiteral TestLiteral{"HSH_PROFILE_MODE"};
    if (StringRef(Def).startswith(TestLiteral) &&
        (Def.size() == TestLiteral.size() || Def[TestLiteral.size()] == '='))
      continue;
    args.emplace_back("-D");
    args.push_back(Def);
  }
  if (MD) {
    args.emplace_back("-MD");
    if (!MF.empty()) {
      args.emplace_back("-MF");
      args.push_back(MF);
    }
    if (!MT.empty()) {
      args.emplace_back("-MT");
      args.push_back(MT);
    }
  }
  args.emplace_back("-o");
  args.push_back(Output);
  args.push_back(Input);

  llvm::SmallVector<hshgen::HshTarget, 4> Targets;
  for (const auto &T : HshTargets) {
    if (T)
      Targets.push_back(T.Target);
  }
  if (Targets.empty()) {
    const std::string ProgramName = sys::path::filename(argv[0]);
    errs() << ProgramName << ": No hsh targets specified!\n"
           << "Must specify at least one of --glsl, --hlsl, --metal, etc...\n"
           << "See: " << argv[0] << " --help\n";
    return 1;
  }

  llvm::IntrusiveRefCntPtr<FileManager> fman(
      new FileManager(FileSystemOptions()));
  tooling::ToolInvocation TI(std::move(args),
                             std::make_unique<hshgen::GenerateAction>(Targets),
                             fman.get());
  if (!TI.run())
    return 1;

  return 0;
}
