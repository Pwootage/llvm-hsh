//===--- HshGenerator.cpp - Codegen for hshgen tool -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Config/config.h"
#include "llvm/IR/Value.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/raw_carray_ostream.h"
#include "llvm/Support/raw_comment_ostream.h"
#include "llvm/Support/xxhash.h"

#include "clang/AST/ASTDumper.h"
#include "clang/AST/CXXInheritance.h"
#include "clang/AST/DeclVisitor.h"
#include "clang/AST/GlobalDecl.h"
#include "clang/AST/QualTypeNames.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/Analysis/AnalysisDeclContext.h"
#include "clang/Basic/FileManager.h"
#include "clang/Config/config.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Hsh/HshGenerator.h"
#include "clang/Lex/MacroArgs.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Parse/Parser.h"

#include "dxc/dxcapi.h"

#include "compiler_iface.h"

#define XSTR(X) #X
#define STR(X) XSTR(X)

#define ENABLE_DUMP 0

namespace clang::hshgen {

using namespace llvm;
using namespace clang;
using namespace clang::hshgen;
using namespace std::literals;

}