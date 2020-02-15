//===--- HshGenerator.cpp - Lambda scanner and codegen for hsh tool -------===//
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
#include "clang/AST/DeclVisitor.h"
#include "clang/AST/GlobalDecl.h"
#include "clang/AST/QualTypeNames.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Hsh/HshGenerator.h"
#include "clang/Lex/MacroArgs.h"
#include "clang/Lex/PreprocessorOptions.h"

#include "dxc/dxcapi.h"

#include <unordered_set>

#define XSTR(X) #X
#define STR(X) XSTR(X)

namespace clang::hshgen {

using namespace llvm;
using namespace clang;
using namespace clang::ast_matchers;
using namespace std::literals;

#ifdef __EMULATE_UUID
#define HSH_IID_PPV_ARGS(ppType)                                               \
  DxcLibrary::SharedInstance->UUIDs.get<std::decay_t<decltype(**(ppType))>>(), \
      reinterpret_cast<void **>(ppType)
#else // __EMULATE_UUID
#define HSH_IID_PPV_ARGS(ppType)                                               \
  __uuidof(**(ppType)), IID_PPV_ARGS_Helper(ppType)
#endif // __EMULATE_UUID

class DxcLibrary {
  sys::DynamicLibrary Library;
  DxcCreateInstanceProc DxcCreateInstance;

public:
  static llvm::Optional<DxcLibrary> SharedInstance;
  static void EnsureSharedInstance(StringRef ProgramDir,
                                   DiagnosticsEngine &Diags) {
    if (!SharedInstance)
      SharedInstance.emplace(ProgramDir, Diags);
  }

#ifdef __EMULATE_UUID
  struct ImportedUUIDs {
    void *_IUnknown = nullptr;
    void *_IDxcBlob = nullptr;
    void *_IDxcBlobUtf8 = nullptr;
    void *_IDxcResult = nullptr;
    void *_IDxcCompiler3 = nullptr;
    void import(sys::DynamicLibrary &Library) {
      _IUnknown = Library.getAddressOfSymbol("_ZN8IUnknown11IUnknown_IDE");
      _IDxcBlob = Library.getAddressOfSymbol("_ZN8IDxcBlob11IDxcBlob_IDE");
      _IDxcBlobUtf8 =
          Library.getAddressOfSymbol("_ZN12IDxcBlobUtf815IDxcBlobUtf8_IDE");
      _IDxcResult =
          Library.getAddressOfSymbol("_ZN10IDxcResult13IDxcResult_IDE");
      _IDxcCompiler3 =
          Library.getAddressOfSymbol("_ZN13IDxcCompiler316IDxcCompiler3_IDE");
    }
    template <typename T> REFIID get();
  } UUIDs;
#endif

  explicit DxcLibrary(StringRef ProgramDir, DiagnosticsEngine &Diags) {
    SmallString<64> LibPath(ProgramDir);
    sys::path::append(LibPath, "libdxcompiler" LTDL_SHLIB_EXT);
    std::string Err;
    Library = sys::DynamicLibrary::getPermanentLibrary(LibPath.c_str(), &Err);
    if (!Library.isValid()) {
      Diags.Report(Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                         "unable to load %0; %1"))
          << LibPath << Err;
      return;
    }
    DxcCreateInstance = reinterpret_cast<DxcCreateInstanceProc>(
        Library.getAddressOfSymbol("DxcCreateInstance"));
    if (!DxcCreateInstance) {
      Diags.Report(Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                         "unable to find DxcCreateInstance"));
      return;
    }
#ifdef __EMULATE_UUID
    UUIDs.import(Library);
#endif
  }

  CComPtr<IDxcCompiler3> MakeCompiler() const;
};
llvm::Optional<DxcLibrary> DxcLibrary::SharedInstance;

#ifdef __EMULATE_UUID
template <> REFIID DxcLibrary::ImportedUUIDs::get<IUnknown>() {
  return _IUnknown;
}
template <> REFIID DxcLibrary::ImportedUUIDs::get<IDxcBlob>() {
  return _IDxcBlob;
}
template <> REFIID DxcLibrary::ImportedUUIDs::get<IDxcBlobUtf8>() {
  return _IDxcBlobUtf8;
}
template <> REFIID DxcLibrary::ImportedUUIDs::get<IDxcResult>() {
  return _IDxcResult;
}
template <> REFIID DxcLibrary::ImportedUUIDs::get<IDxcCompiler3>() {
  return _IDxcCompiler3;
}
#endif

CComPtr<IDxcCompiler3> DxcLibrary::MakeCompiler() const {
  CComPtr<IDxcCompiler3> Ret;
  DxcCreateInstance(CLSID_DxcCompiler, HSH_IID_PPV_ARGS(&Ret));
  return Ret;
}

#define HSH_MAX_VERTEX_BUFFERS 32
#define HSH_MAX_TEXTURES 32
#define HSH_MAX_SAMPLERS 32
#define HSH_MAX_COLOR_TARGETS 8

enum HshStage : int {
  HshNoStage = -1,
  HshHostStage = 0,
  HshVertexStage,
  HshControlStage,
  HshEvaluationStage,
  HshGeometryStage,
  HshFragmentStage,
  HshMaxStage
};

static StringRef HshStageToString(HshStage Stage) {
  switch (Stage) {
  case HshHostStage:
    return llvm::StringLiteral("host");
  case HshVertexStage:
    return llvm::StringLiteral("vertex");
  case HshControlStage:
    return llvm::StringLiteral("control");
  case HshEvaluationStage:
    return llvm::StringLiteral("evaluation");
  case HshGeometryStage:
    return llvm::StringLiteral("geometry");
  case HshFragmentStage:
    return llvm::StringLiteral("fragment");
  default:
    return llvm::StringLiteral("none");
  }
}

enum HshAttributeKind { PerVertex, PerInstance };

enum HshFormat : uint8_t {
  R8_UNORM,
  RG8_UNORM,
  RGB8_UNORM,
  RGBA8_UNORM,
  R16_UNORM,
  RG16_UNORM,
  RGB16_UNORM,
  RGBA16_UNORM,
  R8_SNORM,
  RG8_SNORM,
  RGB8_SNORM,
  RGBA8_SNORM,
  R16_SNORM,
  RG16_SNORM,
  RGB16_SNORM,
  RGBA16_SNORM,
  R32_SFLOAT,
  RG32_SFLOAT,
  RGB32_SFLOAT,
  RGBA32_SFLOAT,
};

enum HshBuiltinType {
  HBT_None,
#define BUILTIN_VECTOR_TYPE(Name, GLSL, HLSL, Metal) HBT_##Name,
#define BUILTIN_MATRIX_TYPE(Name, GLSL, HLSL, Metal) HBT_##Name,
#define BUILTIN_TEXTURE_TYPE(Name, GLSLf, GLSLi, GLSLu, HLSLf, HLSLi, HLSLu,   \
                             Metalf, Metali, Metalu)                           \
  HBT_##Name##_float, HBT_##Name##_int, HBT_##Name##_uint,
#include "BuiltinTypes.def"
  HBT_Max
};

enum HshBuiltinFunction {
  HBF_None,
#define BUILTIN_FUNCTION(Name, GLSL, HLSL, Metal, InterpDist) HBF_##Name,
#include "BuiltinFunctions.def"
  HBF_Max
};

enum HshBuiltinCXXMethod {
  HBM_None,
#define BUILTIN_CXX_METHOD(Name, IsSwizzle, Record, ...) HBM_##Name##_##Record,
#include "BuiltinCXXMethods.def"
  HBM_Max
};

class HshBuiltins {
public:
  struct Spellings {
    StringRef GLSL, HLSL, Metal;
  };

private:
  ClassTemplateDecl *BaseRecordType = nullptr;
  FunctionTemplateDecl *PushUniformMethod = nullptr;
  EnumDecl *EnumTarget = nullptr;
  EnumDecl *EnumStage = nullptr;
  EnumDecl *EnumInputRate = nullptr;
  EnumDecl *EnumFormat = nullptr;
  ClassTemplateDecl *ShaderDataTemplateType = nullptr;
  CXXRecordDecl *GlobalListNodeRecordType = nullptr;
  std::array<const TagDecl *, HBT_Max> Types{};
  std::array<const FunctionDecl *, HBF_Max> Functions{};
  std::array<const CXXMethodDecl *, HBM_Max> Methods{};

  static void printEnumeratorString(raw_ostream &Out,
                                    const PrintingPolicy &Policy, EnumDecl *ED,
                                    const APSInt &Val) {
    for (const EnumConstantDecl *ECD : ED->enumerators()) {
      if (llvm::APSInt::isSameValue(ECD->getInitVal(), Val)) {
        ECD->printQualifiedName(Out, Policy);
        return;
      }
    }
  }

  static constexpr Spellings BuiltinTypeSpellings[] = {
      {{}, {}, {}},
#define BUILTIN_VECTOR_TYPE(Name, GLSL, HLSL, Metal)                           \
  {llvm::StringLiteral(#GLSL), llvm::StringLiteral(#HLSL),                     \
   llvm::StringLiteral(#Metal)},
#define BUILTIN_MATRIX_TYPE(Name, GLSL, HLSL, Metal)                           \
  {llvm::StringLiteral(#GLSL), llvm::StringLiteral(#HLSL),                     \
   llvm::StringLiteral(#Metal)},
#define BUILTIN_TEXTURE_TYPE(Name, GLSLf, GLSLi, GLSLu, HLSLf, HLSLi, HLSLu,   \
                             Metalf, Metali, Metalu)                           \
  {llvm::StringLiteral(#GLSLf), llvm::StringLiteral(#HLSLf),                   \
   llvm::StringLiteral(#Metalf)},                                              \
      {llvm::StringLiteral(#GLSLi), llvm::StringLiteral(#HLSLi),               \
       llvm::StringLiteral(#Metali)},                                          \
      {llvm::StringLiteral(#GLSLu), llvm::StringLiteral(#HLSLu),               \
       llvm::StringLiteral(#Metalu)},
#include "BuiltinTypes.def"
  };

  static constexpr bool BuiltinTypeVector[] = {
      false,
#define BUILTIN_VECTOR_TYPE(Name, GLSL, HLSL, Metal) true,
#define BUILTIN_MATRIX_TYPE(Name, GLSL, HLSL, Metal) false,
#define BUILTIN_TEXTURE_TYPE(Name, GLSLf, GLSLi, GLSLu, HLSLf, HLSLi, HLSLu,   \
                             Metalf, Metali, Metalu)                           \
  false, false, false,
#include "BuiltinTypes.def"
  };

  static constexpr bool BuiltinTypeMatrix[] = {
      false,
#define BUILTIN_VECTOR_TYPE(Name, GLSL, HLSL, Metal) false,
#define BUILTIN_MATRIX_TYPE(Name, GLSL, HLSL, Metal) true,
#define BUILTIN_TEXTURE_TYPE(Name, GLSLf, GLSLi, GLSLu, HLSLf, HLSLi, HLSLu,   \
                             Metalf, Metali, Metalu)                           \
  false, false, false,
#include "BuiltinTypes.def"
  };

  static constexpr bool BuiltinTypeTexture[] = {
      false,
#define BUILTIN_VECTOR_TYPE(Name, GLSL, HLSL, Metal) false,
#define BUILTIN_MATRIX_TYPE(Name, GLSL, HLSL, Metal) false,
#define BUILTIN_TEXTURE_TYPE(Name, GLSLf, GLSLi, GLSLu, HLSLf, HLSLi, HLSLu,   \
                             Metalf, Metali, Metalu)                           \
  true, true, true,
#include "BuiltinTypes.def"
  };

  static constexpr bool BuiltinMethodSwizzle[] = {
      false,
#define BUILTIN_CXX_METHOD(Name, IsSwizzle, Record, ...) IsSwizzle,
#include "BuiltinCXXMethods.def"
  };

  static constexpr Spellings BuiltinFunctionSpellings[] = {
      {{}, {}, {}},
#define BUILTIN_FUNCTION(Name, GLSL, HLSL, Metal, InterpDist)                  \
  {llvm::StringLiteral(#GLSL), llvm::StringLiteral(#HLSL),                     \
   llvm::StringLiteral(#Metal)},
#include "BuiltinFunctions.def"
  };

  static constexpr bool BuiltinFunctionInterpDists[] = {
      false,
#define BUILTIN_FUNCTION(Name, GLSL, HLSL, Metal, InterpDist) InterpDist,
#include "BuiltinFunctions.def"
  };

  template <typename ImplClass>
  class DeclFinder : public DeclVisitor<ImplClass, bool> {
    using base = DeclVisitor<ImplClass, bool>;

  protected:
    StringRef Name;
    Decl *Found = nullptr;
    bool InHshNS = false;

  public:
    bool VisitDecl(Decl *D) {
      if (auto *DC = dyn_cast<DeclContext>(D))
        for (Decl *Child : DC->decls())
          if (!base::Visit(Child))
            return false;
      return true;
    }

    bool VisitNamespaceDecl(NamespaceDecl *Namespace) {
      if (InHshNS)
        return true;
      bool Ret = true;
      if (Namespace->getDeclName().isIdentifier() &&
          Namespace->getName() == llvm::StringLiteral("hsh")) {
        SaveAndRestore<bool> SavedInHshNS(InHshNS, true);
        Ret = VisitDecl(Namespace);
      }
      return Ret;
    }

    Decl *Find(StringRef N, TranslationUnitDecl *TU) {
      Name = N;
      Found = nullptr;
      base::Visit(TU);
      return Found;
    }
  };

  class TypeFinder : public DeclFinder<TypeFinder> {
  public:
    bool VisitTagDecl(TagDecl *Type) {
      if (InHshNS && Type->getDeclName().isIdentifier() &&
          Type->getName() == Name) {
        Found = Type;
        return false;
      }
      return true;
    }
  };

  class FuncFinder : public DeclFinder<FuncFinder> {
  public:
    bool VisitFunctionDecl(FunctionDecl *Func) {
      if (InHshNS && Func->getDeclName().isIdentifier() &&
          Func->getName() == Name) {
        Found = Func;
        return false;
      }
      return true;
    }
  };

  class ClassTemplateFinder : public DeclFinder<ClassTemplateFinder> {
  public:
    bool VisitClassTemplateDecl(ClassTemplateDecl *Type) {
      if (InHshNS && Type->getDeclName().isIdentifier() &&
          Type->getName() == Name) {
        Found = Type;
        return false;
      }
      return true;
    }
  };

  class MethodFinder : public DeclFinder<MethodFinder> {
    StringRef Record;
    SmallVector<StringRef, 8> Params;

  public:
    bool VisitClassTemplateDecl(ClassTemplateDecl *ClassTemplate) {
      return VisitDecl(ClassTemplate->getTemplatedDecl());
    }

    bool VisitCXXMethodDecl(CXXMethodDecl *Method) {
      if (InHshNS && Method->getDeclName().isIdentifier() &&
          Method->getName() == Name &&
          Method->getParent()->getName() == Record &&
          Method->getNumParams() == Params.size()) {
        auto It = Params.begin();
        for (ParmVarDecl *P : Method->parameters()) {
          if (P->getType().getAsString() != *It++)
            return true;
        }
        Found = Method;
        return false;
      }
      return true;
    }

    Decl *Find(StringRef N, StringRef R, StringRef P, TranslationUnitDecl *TU) {
      Name = N;
      Record = R;
      if (P != "void") {
        P.split(Params, ',');
        for (auto &ParamStr : Params)
          ParamStr = ParamStr.trim();
      }
      Found = nullptr;
      Visit(TU);
      return Found;
    }
  };

  void addType(SourceManager &SM, HshBuiltinType TypeKind, StringRef Name,
               Decl *D) {
    if (auto *T = dyn_cast_or_null<TagDecl>(D)) {
      Types[TypeKind] = T->getFirstDecl();
    } else {
      DiagnosticsEngine &Diags = SM.getDiagnostics();
      Diags.Report(Diags.getCustomDiagID(
          DiagnosticsEngine::Error, "unable to locate declaration of builtin "
                                    "type %0; is hsh.h included?"))
          << Name;
    }
  }

  void addTextureType(SourceManager &SM, HshBuiltinType FirstEnum,
                      StringRef Name, Decl *D) {
    DiagnosticsEngine &Diags = SM.getDiagnostics();
    if (auto *T = dyn_cast_or_null<ClassTemplateDecl>(D)) {
      for (const auto *Spec : T->specializations()) {
        QualType Tp = Spec->getTemplateArgs()[0].getAsType();
        if (Tp->isSpecificBuiltinType(BuiltinType::Float)) {
          Types[FirstEnum + 0] = Spec;
        } else if (Tp->isSpecificBuiltinType(BuiltinType::Int)) {
          Types[FirstEnum + 1] = Spec;
        } else if (Tp->isSpecificBuiltinType(BuiltinType::UInt)) {
          Types[FirstEnum + 2] = Spec;
        } else {
          Diags.Report(
              Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                    "unknown texture specialization type "
                                    "%0; must use float, int, unsigned int"))
              << Tp.getAsString();
        }
      }
    } else {
      Diags.Report(Diags.getCustomDiagID(
          DiagnosticsEngine::Error, "unable to locate declaration of builtin "
                                    "class template %0; is hsh.h included?"))
          << Name;
    }
  }

  void addFunction(SourceManager &SM, HshBuiltinFunction FuncKind,
                   StringRef Name, Decl *D) {
    if (auto *F = dyn_cast_or_null<FunctionDecl>(D)) {
      Functions[FuncKind] = F->getFirstDecl();
    } else {
      DiagnosticsEngine &Diags = SM.getDiagnostics();
      Diags.Report(Diags.getCustomDiagID(
          DiagnosticsEngine::Error, "unable to locate declaration of builtin "
                                    "function %0; is hsh.h included?"))
          << Name;
    }
  }

  void addCXXMethod(SourceManager &SM, HshBuiltinCXXMethod MethodKind,
                    StringRef Name, Decl *D) {
    if (auto *M = dyn_cast_or_null<CXXMethodDecl>(D)) {
      Methods[MethodKind] = dyn_cast<CXXMethodDecl>(M->getFirstDecl());
    } else {
      DiagnosticsEngine &Diags = SM.getDiagnostics();
      Diags.Report(Diags.getCustomDiagID(
          DiagnosticsEngine::Error, "unable to locate declaration of builtin "
                                    "method %0; is hsh.h included?"))
          << Name;
    }
  }

public:
  void findBuiltinDecls(ASTContext &Context) {
    DiagnosticsEngine &Diags = Context.getDiagnostics();
    TranslationUnitDecl *TU = Context.getTranslationUnitDecl();
    SourceManager &SM = Context.getSourceManager();
    if (auto *T = dyn_cast_or_null<ClassTemplateDecl>(
            ClassTemplateFinder().Find(llvm::StringLiteral("_HshBase"), TU))) {
      BaseRecordType = cast<ClassTemplateDecl>(T->getFirstDecl());
      auto *TemplDecl = BaseRecordType->getTemplatedDecl();
      using FuncTemplIt =
          CXXRecordDecl::specific_decl_iterator<FunctionTemplateDecl>;
      for (FuncTemplIt TI(TemplDecl->decls_begin()), TE(TemplDecl->decls_end());
           TI != TE; ++TI) {
        if (TI->getName() == llvm::StringLiteral("push_uniform"))
          PushUniformMethod = *TI;
      }
      if (!PushUniformMethod) {
        Diags.Report(Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                           "unable to locate declaration of "
                                           "_HshBase::push_uniform; "
                                           "is hsh.h included?"));
      }
    } else {
      Diags.Report(Diags.getCustomDiagID(
          DiagnosticsEngine::Error, "unable to locate declaration of _HshBase; "
                                    "is hsh.h included?"));
    }

    if (auto *E = dyn_cast_or_null<EnumDecl>(
            TypeFinder().Find(llvm::StringLiteral("Target"), TU))) {
      EnumTarget = E;
    } else {
      Diags.Report(
          Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                "unable to locate declaration of enum Target; "
                                "is hsh.h included?"));
    }

    if (auto *E = dyn_cast_or_null<EnumDecl>(
            TypeFinder().Find(llvm::StringLiteral("Stage"), TU))) {
      EnumStage = E;
    } else {
      Diags.Report(
          Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                "unable to locate declaration of enum Stage; "
                                "is hsh.h included?"));
    }

    if (auto *E = dyn_cast_or_null<EnumDecl>(
            TypeFinder().Find(llvm::StringLiteral("_HshInputRate"), TU))) {
      EnumInputRate = E;
    } else {
      Diags.Report(Diags.getCustomDiagID(
          DiagnosticsEngine::Error,
          "unable to locate declaration of enum _HshInputRate; "
          "is hsh.h included?"));
    }

    if (auto *E = dyn_cast_or_null<EnumDecl>(
            TypeFinder().Find(llvm::StringLiteral("_HshFormat"), TU))) {
      EnumFormat = E;
    } else {
      Diags.Report(Diags.getCustomDiagID(
          DiagnosticsEngine::Error,
          "unable to locate declaration of enum _HshFormat; "
          "is hsh.h included?"));
    }

    if (auto *T =
            dyn_cast_or_null<ClassTemplateDecl>(ClassTemplateFinder().Find(
                llvm::StringLiteral("_HshShaderData"), TU))) {
      ShaderDataTemplateType = T;
    } else {
      Diags.Report(Diags.getCustomDiagID(
          DiagnosticsEngine::Error,
          "unable to locate declaration of _HshShaderData; "
          "is hsh.h included?"));
    }

    if (auto *R = dyn_cast_or_null<CXXRecordDecl>(
            TypeFinder().Find(llvm::StringLiteral("_HshGlobalListNode"), TU))) {
      GlobalListNodeRecordType = R;
    } else {
      Diags.Report(Diags.getCustomDiagID(
          DiagnosticsEngine::Error,
          "unable to locate declaration of _HshGlobalListNode; "
          "is hsh.h included?"));
    }

#define BUILTIN_VECTOR_TYPE(Name, GLSL, HLSL, Metal)                           \
  addType(SM, HBT_##Name, llvm::StringLiteral(#Name),                          \
          TypeFinder().Find(llvm::StringLiteral(#Name), TU));
#define BUILTIN_MATRIX_TYPE(Name, GLSL, HLSL, Metal)                           \
  addType(SM, HBT_##Name, llvm::StringLiteral(#Name),                          \
          TypeFinder().Find(llvm::StringLiteral(#Name), TU));
#define BUILTIN_TEXTURE_TYPE(Name, GLSLf, GLSLi, GLSLu, HLSLf, HLSLi, HLSLu,   \
                             Metalf, Metali, Metalu)                           \
  addTextureType(SM, HBT_##Name##_float, llvm::StringLiteral(#Name),           \
                 ClassTemplateFinder().Find(llvm::StringLiteral(#Name), TU));
#include "BuiltinTypes.def"
#define BUILTIN_FUNCTION(Name, GLSL, HLSL, Metal, InterpDist)                  \
  addFunction(SM, HBF_##Name, llvm::StringLiteral(#Name),                      \
              FuncFinder().Find(llvm::StringLiteral(#Name), TU));
#include "BuiltinFunctions.def"
#define BUILTIN_CXX_METHOD(Name, IsSwizzle, Record, ...)                       \
  addCXXMethod(SM, HBM_##Name##_##Record,                                      \
               llvm::StringLiteral(#Record "::" #Name "(" #__VA_ARGS__ ")"),   \
               MethodFinder().Find(llvm::StringLiteral(#Name),                 \
                                   llvm::StringLiteral(#Record),               \
                                   llvm::StringLiteral(#__VA_ARGS__), TU));
#include "BuiltinCXXMethods.def"
  }

  HshBuiltinType identifyBuiltinType(QualType QT) const {
    return identifyBuiltinType(QT.getNonReferenceType().getTypePtrOrNull());
  }

  HshBuiltinType identifyBuiltinType(const clang::Type *UT) const {
    if (!UT)
      return HBT_None;
    TagDecl *T = UT->getAsTagDecl();
    if (!T)
      return HBT_None;
    T = T->getFirstDecl();
    if (!T)
      return HBT_None;
    HshBuiltinType Ret = HBT_None;
    for (const auto *Tp : Types) {
      if (T == Tp)
        return Ret;
      Ret = HshBuiltinType(int(Ret) + 1);
    }
    return HBT_None;
  }

  HshBuiltinFunction
  identifyBuiltinFunction(const clang::FunctionDecl *F) const {
    F = F->getFirstDecl();
    if (!F)
      return HBF_None;
    HshBuiltinFunction Ret = HBF_None;
    for (const auto *Func : Functions) {
      if (F == Func)
        return Ret;
      Ret = HshBuiltinFunction(int(Ret) + 1);
    }
    return HBF_None;
  }

  HshBuiltinCXXMethod
  identifyBuiltinMethod(const clang::CXXMethodDecl *M) const {
    M = dyn_cast_or_null<CXXMethodDecl>(M->getFirstDecl());
    if (!M)
      return HBM_None;
    if (FunctionDecl *FD = M->getInstantiatedFromMemberFunction())
      M = dyn_cast<CXXMethodDecl>(FD->getFirstDecl());
    HshBuiltinCXXMethod Ret = HBM_None;
    for (const auto *Method : Methods) {
      if (M == Method)
        return Ret;
      Ret = HshBuiltinCXXMethod(int(Ret) + 1);
    }
    return HBM_None;
  }

  static constexpr const Spellings &getSpellings(HshBuiltinType Tp) {
    return BuiltinTypeSpellings[Tp];
  }

  template <HshTarget T>
  static constexpr StringRef getSpelling(HshBuiltinType Tp);

  static constexpr const Spellings &getSpellings(HshBuiltinFunction Func) {
    return BuiltinFunctionSpellings[Func];
  }

  template <HshTarget T>
  static constexpr StringRef getSpelling(HshBuiltinFunction Func);

  static constexpr bool isVectorType(HshBuiltinType Tp) {
    return BuiltinTypeVector[Tp];
  }

  static constexpr bool isMatrixType(HshBuiltinType Tp) {
    return BuiltinTypeMatrix[Tp];
  }

  static constexpr bool isTextureType(HshBuiltinType Tp) {
    return BuiltinTypeTexture[Tp];
  }

  static constexpr bool isSwizzleMethod(HshBuiltinCXXMethod M) {
    return BuiltinMethodSwizzle[M];
  }

  static constexpr bool isInterpolationDistributed(HshBuiltinFunction Func) {
    return BuiltinFunctionInterpDists[Func];
  }

  const clang::TagDecl *getTypeDecl(HshBuiltinType Tp) const {
    return Types[Tp];
  }

  QualType getType(HshBuiltinType Tp) const {
    return getTypeDecl(Tp)->getTypeForDecl()->getCanonicalTypeUnqualified();
  }

  static TypeSourceInfo *getFullyQualifiedTemplateSpecializationTypeInfo(
      ASTContext &Context, TemplateDecl *TDecl,
      const TemplateArgumentListInfo &Args) {
    QualType Underlying =
        Context.getTemplateSpecializationType(TemplateName(TDecl), Args);
    Underlying = TypeName::getFullyQualifiedType(Underlying, Context);
    return Context.getTrivialTypeSourceInfo(Underlying);
  }

  CXXRecordDecl *getHshBaseSpecialization(ASTContext &Context,
                                          StringRef Name) const {
    CXXRecordDecl *Record = CXXRecordDecl::Create(
        Context, TTK_Class, Context.getTranslationUnitDecl(), {}, {},
        &Context.Idents.get(Name));
    Record->startDefinition();

    TemplateArgumentListInfo TemplateArgs;
    TemplateArgs.addArgument(TemplateArgumentLoc(
        QualType{Record->getTypeForDecl(), 0}, (TypeSourceInfo *)nullptr));
    TypeSourceInfo *TSI = getFullyQualifiedTemplateSpecializationTypeInfo(
        Context, BaseRecordType, TemplateArgs);
    CXXBaseSpecifier BaseSpec({}, false, true, AS_public, TSI, {});
    CXXBaseSpecifier *Bases = &BaseSpec;
    Record->setBases(&Bases, 1);

    return Record;
  }

  QualType getHshShaderDataSpecializationType(ASTContext &Context) const {
    auto *NTTP = cast<NonTypeTemplateParmDecl>(
        ShaderDataTemplateType->getTemplateParameters()->getParam(0));
    TemplateArgumentListInfo TemplateArgs;
    TemplateArgs.addArgument(TemplateArgumentLoc(
        TemplateArgument{NTTP, NTTP->getType()}, (Expr *)nullptr));
    TypeSourceInfo *TSI = getFullyQualifiedTemplateSpecializationTypeInfo(
        Context, ShaderDataTemplateType, TemplateArgs);
    return TSI->getType();
  }

  CXXMemberCallExpr *getPushUniformCall(ASTContext &Context, VarDecl *Decl,
                                        HshStage Stage) const {
    auto *NTTP = cast<NonTypeTemplateParmDecl>(
        PushUniformMethod->getTemplateParameters()->getParam(0));
    TemplateArgument TemplateArg{Context, APSInt::get(int(Stage) - 1),
                                 NTTP->getType()};
    auto *PushUniform =
        cast<CXXMethodDecl>(PushUniformMethod->getTemplatedDecl());
    TemplateArgumentListInfo CallTemplArgs(PushUniformMethod->getLocation(),
                                           {});
    CallTemplArgs.addArgument(
        TemplateArgumentLoc(TemplateArg, (Expr *)nullptr));
    auto *ThisExpr = new (Context) CXXThisExpr({}, Context.VoidTy, true);
    auto *ME = MemberExpr::Create(
        Context, ThisExpr, true, {}, {}, {}, PushUniform,
        DeclAccessPair::make(PushUniform, PushUniform->getAccess()), {},
        &CallTemplArgs, Context.VoidTy, VK_XValue, OK_Ordinary, NOUR_None);
    Expr *Arg =
        DeclRefExpr::Create(Context, {}, {}, Decl, false, SourceLocation{},
                            Decl->getType(), VK_XValue);
    return CXXMemberCallExpr::Create(Context, ME, Arg, Context.VoidTy,
                                     VK_XValue, {});
  }

  VarTemplateDecl *getDataVarTemplate(ASTContext &Context, DeclContext *DC,
                                      uint32_t NumStages, uint32_t NumBindings,
                                      uint32_t NumAttributes) const {
    NonTypeTemplateParmDecl *TargetParm = NonTypeTemplateParmDecl::Create(
        Context, DC, {}, {}, 0, 0, &Context.Idents.get("T"),
        QualType{EnumTarget->getTypeForDecl(), 0}, false, nullptr);
    auto *TPL =
        TemplateParameterList::Create(Context, {}, {}, TargetParm, {}, nullptr);
    auto *PExpr =
        DeclRefExpr::Create(Context, {}, {}, TargetParm, false,
                            SourceLocation{}, TargetParm->getType(), VK_XValue);
    TemplateArgumentListInfo TemplateArgs;
    TemplateArgs.addArgument(
        TemplateArgumentLoc(TemplateArgument{PExpr}, PExpr));
    TemplateArgs.addArgument(
        TemplateArgumentLoc(TemplateArgument{Context, APSInt::get(NumStages),
                                             Context.UnsignedIntTy},
                            (Expr *)nullptr));
    TemplateArgs.addArgument(
        TemplateArgumentLoc(TemplateArgument{Context, APSInt::get(NumBindings),
                                             Context.UnsignedIntTy},
                            (Expr *)nullptr));
    TemplateArgs.addArgument(TemplateArgumentLoc(
        TemplateArgument{Context, APSInt::get(NumAttributes),
                         Context.UnsignedIntTy},
        (Expr *)nullptr));
    TypeSourceInfo *TSI = getFullyQualifiedTemplateSpecializationTypeInfo(
        Context, ShaderDataTemplateType, TemplateArgs);

    auto *VD = VarDecl::Create(Context, DC, {}, {}, &Context.Idents.get("data"),
                               TSI->getType(), nullptr, SC_Static);
    VD->setInitStyle(VarDecl::ListInit);
    VD->setInit(new (Context) InitListExpr(Stmt::EmptyShell{}));
    return VarTemplateDecl::Create(Context, DC, {}, VD->getIdentifier(), TPL,
                                   VD);
  }

  VarDecl *getGlobalListNode(ASTContext &Context, DeclContext *DC) const {
    return VarDecl::Create(
        Context, DC, {}, {}, &Context.Idents.get("global"),
        QualType{GlobalListNodeRecordType->getTypeForDecl(), 0}, nullptr,
        SC_Static);
  }

  void printTargetEnumString(raw_ostream &Out, const PrintingPolicy &Policy,
                             HshTarget Target) const {
    printEnumeratorString(Out, Policy, EnumTarget, APSInt::get(Target));
  }

  void printStageEnumString(raw_ostream &Out, const PrintingPolicy &Policy,
                            HshStage Stage) const {
    printEnumeratorString(Out, Policy, EnumStage, APSInt::get(Stage - 1));
  }

  void printInputRateEnumString(raw_ostream &Out, const PrintingPolicy &Policy,
                                HshAttributeKind InputRate) const {
    printEnumeratorString(Out, Policy, EnumInputRate, APSInt::get(InputRate));
  }

  void printFormatEnumString(raw_ostream &Out, const PrintingPolicy &Policy,
                             HshFormat Format) const {
    printEnumeratorString(Out, Policy, EnumFormat, APSInt::get(Format));
  }
};

template <>
constexpr StringRef HshBuiltins::getSpelling<HT_GLSL>(HshBuiltinType Tp) {
  return getSpellings(Tp).GLSL;
}
template <>
constexpr StringRef HshBuiltins::getSpelling<HT_HLSL>(HshBuiltinType Tp) {
  return getSpellings(Tp).HLSL;
}
template <>
constexpr StringRef HshBuiltins::getSpelling<HT_METAL>(HshBuiltinType Tp) {
  return getSpellings(Tp).Metal;
}

template <>
constexpr StringRef HshBuiltins::getSpelling<HT_GLSL>(HshBuiltinFunction Func) {
  return getSpellings(Func).GLSL;
}
template <>
constexpr StringRef HshBuiltins::getSpelling<HT_HLSL>(HshBuiltinFunction Func) {
  return getSpellings(Func).HLSL;
}
template <>
constexpr StringRef
HshBuiltins::getSpelling<HT_METAL>(HshBuiltinFunction Func) {
  return getSpellings(Func).Metal;
}

enum HshInterfaceDirection { HshInput, HshOutput, HshInOut };

static HshStage DetermineParmVarStage(ParmVarDecl *D) {
#define INTERFACE_VARIABLE(Attr, Stage, Direction, Array)                      \
  if (D->hasAttr<Attr>())                                                      \
    return Stage;
#include "ShaderInterface.def"
  return HshHostStage;
}

static HshInterfaceDirection DetermineParmVarDirection(ParmVarDecl *D) {
#define INTERFACE_VARIABLE(Attr, Stage, Direction, Array)                      \
  if (D->hasAttr<Attr>())                                                      \
    return Direction;
#include "ShaderInterface.def"
  return HshInput;
}

template <typename T, unsigned N>
static DiagnosticBuilder
ReportCustom(const T *S, const ASTContext &Context,
             const char (&FormatString)[N],
             DiagnosticsEngine::Level level = DiagnosticsEngine::Error) {
  DiagnosticsEngine &Diags = Context.getDiagnostics();
  return Diags.Report(S->getBeginLoc(),
                      Diags.getCustomDiagID(level, FormatString))
         << CharSourceRange(S->getSourceRange(), false);
}

static void ReportUnsupportedStmt(const Stmt *S, const ASTContext &Context) {
  auto Diag = ReportCustom(
      S, Context,
      "statements of type %0 are not supported in hsh generator lambdas");
  Diag.AddString(S->getStmtClassName());
}

static void ReportUnsupportedFunctionCall(const Stmt *S,
                                          const ASTContext &Context) {
  ReportCustom(S, Context, "function calls are limited to hsh intrinsics");
}

static void ReportUnsupportedTypeReference(const Stmt *S,
                                           const ASTContext &Context) {
  ReportCustom(S, Context, "references to values are limited to hsh types");
}

static void ReportUnsupportedTypeConstruct(const Stmt *S,
                                           const ASTContext &Context) {
  ReportCustom(S, Context, "constructors are limited to hsh types");
}

static void ReportUnsupportedTypeCast(const Stmt *S,
                                      const ASTContext &Context) {
  ReportCustom(S, Context, "type casts are limited to hsh types");
}

static void ReportBadTextureReference(const Stmt *S,
                                      const ASTContext &Context) {
  ReportCustom(S, Context,
               "texture samples must be performed on lambda parameters");
}

static void ReportUnattributedTexture(const ParmVarDecl *PVD,
                                      const ASTContext &Context) {
  ReportCustom(
      PVD, Context,
      "sampled textures must be attributed with [[hsh::*_texture(n)]]");
}

static void ReportNonConstexprSampler(const Expr *E,
                                      const ASTContext &Context) {
  ReportCustom(E, Context, "sampler arguments must be constexpr");
}

static void ReportBadSamplerStructFormat(const Expr *E,
                                         const ASTContext &Context) {
  ReportCustom(E, Context, "sampler structure is not consistent");
}

static void ReportSamplerOverflow(const Expr *E, const ASTContext &Context) {
  ReportCustom(E, Context,
               "maximum sampler limit of " STR(HSH_MAX_SAMPLERS) " reached");
}

static void ReportBadVertexPositionType(const ParmVarDecl *PVD,
                                        const ASTContext &Context) {
  ReportCustom(PVD, Context, "vertex position must be a hsh::float4");
}

static void ReportBadColorTargetType(const ParmVarDecl *PVD,
                                     const ASTContext &Context) {
  ReportCustom(PVD, Context, "fragment color target must be a hsh::float4");
}

static void ReportBadVertexBufferType(const ParmVarDecl *PVD,
                                      const ASTContext &Context) {
  ReportCustom(PVD, Context, "vertex buffer must be a struct or class");
}

static void ReportVertexBufferOutOfRange(const ParmVarDecl *PVD,
                                         const ASTContext &Context) {
  ReportCustom(PVD, Context,
               "vertex buffer index must be in range [0," STR(
                   HSH_MAX_VERTEX_BUFFERS) ")");
}

static void ReportVertexBufferDuplicate(const ParmVarDecl *PVD,
                                        const ParmVarDecl *OtherPVD,
                                        const ASTContext &Context) {
  ReportCustom(PVD, Context, "vertex buffer index be unique");
  ReportCustom(OtherPVD, Context, "previous buffer index here",
               DiagnosticsEngine::Note);
}

static void ReportBadTextureType(const ParmVarDecl *PVD,
                                 const ASTContext &Context) {
  ReportCustom(PVD, Context, "texture must be a texture* type");
}

static void ReportTextureOutOfRange(const ParmVarDecl *PVD,
                                    const ASTContext &Context) {
  ReportCustom(PVD, Context,
               "texture index must be in range [0," STR(HSH_MAX_TEXTURES) ")");
}

static void ReportTextureDuplicate(const ParmVarDecl *PVD,
                                   const ParmVarDecl *OtherPVD,
                                   const ASTContext &Context) {
  ReportCustom(PVD, Context, "texture index be unique");
  ReportCustom(OtherPVD, Context, "previous texture index here",
               DiagnosticsEngine::Note);
}

static void ReportBadIntegerType(const Decl *D, const ASTContext &Context) {
  ReportCustom(D, Context, "integers must be 32-bits in length");
}

static void ReportBadRecordType(const Decl *D, const ASTContext &Context) {
  ReportCustom(D, Context,
               "hsh record fields must be a builtin hsh vector or matrix, "
               "float, double, or 32-bit integer");
}

static void ReportColorTargetOutOfRange(const ParmVarDecl *PVD,
                                        const ASTContext &Context) {
  ReportCustom(
      PVD, Context,
      "color target index must be in range [0," STR(HSH_MAX_COLOR_TARGETS) ")");
}

static bool CheckHshFieldTypeCompatibility(const HshBuiltins &Builtins,
                                           const ASTContext &Context,
                                           const ValueDecl *VD) {
  QualType Tp = VD->getType();
  HshBuiltinType HBT = Builtins.identifyBuiltinType(Tp);
  if (HBT != HBT_None && !HshBuiltins::isTextureType(HBT)) {
    return true;
  } else if (Tp->isIntegralOrEnumerationType()) {
    if (Context.getIntWidth(Tp) != 32) {
      ReportBadIntegerType(VD, Context);
      return false;
    }
  } else if (Tp->isSpecificBuiltinType(BuiltinType::Float) ||
             Tp->isSpecificBuiltinType(BuiltinType::Double)) {
    return true;
  }
  ReportBadRecordType(VD, Context);
  return false;
}

static bool CheckHshRecordCompatibility(const HshBuiltins &Builtins,
                                        const ASTContext &Context,
                                        const CXXRecordDecl *Record) {
  bool Ret = true;
  for (const auto *FD : Record->fields())
    if (!CheckHshFieldTypeCompatibility(Builtins, Context, FD))
      Ret = false;
  return Ret;
}

class LastAssignmentFinder : public StmtVisitor<LastAssignmentFinder, bool> {
  const ASTContext &Context;
  VarDecl *Var = nullptr;
  Stmt *End = nullptr;
  Stmt *Assign = nullptr;
  Stmt *LastAssign = nullptr;
  Stmt *CompoundChild = nullptr;
  Stmt *LastCompoundChild = nullptr;
  bool DoVisit(Stmt *S) {
    if (End && S == End)
      return false;
    if (auto *E = dyn_cast<Expr>(S))
      return Visit(E->IgnoreParenCasts());
    else
      return Visit(S);
  }
  void UpdateLastAssign(Stmt *S) {
    LastAssign = S;
    LastCompoundChild = CompoundChild;
  }

public:
  explicit LastAssignmentFinder(const ASTContext &Context) : Context(Context) {}

  bool VisitStmt(Stmt *S) {
    ReportUnsupportedStmt(S, Context);
    return true;
  }

  bool VisitCompoundStmt(CompoundStmt *CompoundStmt) {
    for (Stmt *ChildStmt : CompoundStmt->body()) {
      CompoundChild = ChildStmt;
      if (!DoVisit(ChildStmt))
        return false;
    }
    return true;
  }

  bool VisitDeclStmt(DeclStmt *DeclStmt) {
    for (Decl *D : DeclStmt->getDeclGroup()) {
      if (auto *VD = dyn_cast<VarDecl>(D)) {
        if (Expr *Init = VD->getInit()) {
          if (VD == Var)
            UpdateLastAssign(DeclStmt);
          else if (!DoVisit(Init))
            return false;
        }
      }
    }
    return true;
  }

  static bool VisitNullStmt(NullStmt *) { return true; }

  bool VisitValueStmt(ValueStmt *ValueStmt) {
    return DoVisit(ValueStmt->getExprStmt());
  }

  bool VisitBinaryOperator(BinaryOperator *BinOp) {
    if (BinOp->isAssignmentOp()) {
      {
        SaveAndRestore<Stmt *> SavedAssign(Assign, BinOp);
        if (!DoVisit(BinOp->getLHS()))
          return false;
      }
      if (!DoVisit(BinOp->getRHS()))
        return false;
    } else {
      if (!DoVisit(BinOp->getLHS()))
        return false;
      if (!DoVisit(BinOp->getRHS()))
        return false;
    }
    return true;
  }

  bool VisitUnaryOperator(UnaryOperator *UnOp) {
    return DoVisit(UnOp->getSubExpr());
  }

  bool VisitExpr(Expr *E) {
    ReportUnsupportedStmt(E, Context);
    return true;
  }

  bool VisitBlockExpr(BlockExpr *Block) { return DoVisit(Block->getBody()); }

  bool VisitCallExpr(CallExpr *CallExpr) {
    for (Expr *Arg : CallExpr->arguments()) {
      if (!DoVisit(Arg))
        return false;
    }
    return true;
  }

  bool VisitCXXConstructExpr(CXXConstructExpr *ConstructExpr) {
    for (Expr *Arg : ConstructExpr->arguments()) {
      if (!DoVisit(Arg))
        return false;
    }
    return true;
  }

  bool VisitCXXOperatorCallExpr(CXXOperatorCallExpr *CallExpr) {
    if (CallExpr->getNumArgs() >= 1 && CallExpr->isAssignmentOp()) {
      {
        SaveAndRestore<Stmt *> SavedAssign(Assign, CallExpr);
        if (!DoVisit(CallExpr->getArg(0)))
          return false;
      }
      if (!DoVisit(CallExpr->getArg(1)))
        return false;
    } else {
      if (!VisitCallExpr(CallExpr))
        return false;
    }
    return true;
  }

  bool VisitDeclRefExpr(DeclRefExpr *DeclRef) {
    if (Assign && DeclRef->getDecl() == Var)
      UpdateLastAssign(Assign);
    return true;
  }

  bool VisitInitListExpr(InitListExpr *InitList) {
    for (Stmt *S : *InitList) {
      if (!DoVisit(S))
        return false;
    }
    return true;
  }

  bool VisitMemberExpr(MemberExpr *MemberExpr) {
    return DoVisit(MemberExpr->getBase());
  }

  static bool VisitFloatingLiteral(FloatingLiteral *) { return true; }

  static bool VisitIntegerLiteral(IntegerLiteral *) { return true; }

  std::tuple<Stmt *, Stmt *> Find(VarDecl *V, Stmt *Body, Stmt *E = nullptr) {
    Var = V;
    End = E;
    DoVisit(Body);
    return {LastAssign, LastCompoundChild};
  }
};

struct AssignmentFinderInfo {
  Stmt *Body = nullptr;
  Stmt *LastCompoundChild = nullptr;
  VarDecl *SelectedVarDecl = nullptr;
};

enum HshSamplerFilterMode {
  HSF_Linear,
  HSF_Nearest,
};
enum HshSamplerWrapMode {
  HSW_Repeat,
  HSW_Clamp,
};
struct SamplerConfig {
  static constexpr unsigned NumFields = 2;
  static bool ValidateSamplerStruct(const APValue &Val) {
    if (!Val.isStruct() || Val.getStructNumFields() != NumFields)
      return false;
    for (unsigned i = 0; i < NumFields; ++i)
      if (!Val.getStructField(i).isInt())
        return false;
    return true;
  }
  HshSamplerFilterMode Filter = HSF_Linear;
  HshSamplerWrapMode Wrap = HSW_Repeat;
  SamplerConfig() = default;
  explicit SamplerConfig(const APValue &Val) {
    Filter =
        HshSamplerFilterMode(Val.getStructField(0).getInt().getSExtValue());
    Wrap = HshSamplerWrapMode(Val.getStructField(1).getInt().getSExtValue());
  }
  bool operator==(const SamplerConfig &Other) const {
    return Filter == Other.Filter && Wrap == Other.Wrap;
  }
};

struct SamplerRecord {
  SamplerConfig Config;
  unsigned UseStages;
};

struct ColorTargetRecord {
  StringRef Name;
  unsigned Index;
};

template <typename TexAttr> constexpr HshStage StageOfTextureAttr() {
  return HshNoStage;
}
template <> constexpr HshStage StageOfTextureAttr<HshVertexTextureAttr>() {
  return HshVertexStage;
}
template <> constexpr HshStage StageOfTextureAttr<HshFragmentTextureAttr>() {
  return HshFragmentStage;
}

struct AttributeRecord {
  StringRef Name;
  const CXXRecordDecl *Record;
  HshAttributeKind Kind;
  uint8_t Binding;
};

enum HshTextureKind {
#define BUILTIN_TEXTURE_TYPE(Name, GLSLf, GLSLi, GLSLu, HLSLf, HLSLi, HLSLu,   \
                             Metalf, Metali, Metalu)                           \
  HTK_##Name##_float, HTK_##Name##_int, HTK_##Name##_uint,
#include "BuiltinTypes.def"
};

constexpr HshTextureKind KindOfTextureType(HshBuiltinType Type) {
  switch (Type) {
#define BUILTIN_TEXTURE_TYPE(Name, GLSLf, GLSLi, GLSLu, HLSLf, HLSLi, HLSLu,   \
                             Metalf, Metali, Metalu)                           \
  case HBT_##Name##_float:                                                     \
    return HTK_##Name##_float;                                                 \
  case HBT_##Name##_int:                                                       \
    return HTK_##Name##_int;                                                   \
  case HBT_##Name##_uint:                                                      \
    return HTK_##Name##_uint;
#include "BuiltinTypes.def"
  default:
    llvm_unreachable("invalid texture kind");
  }
}

constexpr HshBuiltinType BuiltinTypeOfTexture(HshTextureKind Kind) {
  switch (Kind) {
#define BUILTIN_TEXTURE_TYPE(Name, GLSLf, GLSLi, GLSLu, HLSLf, HLSLi, HLSLu,   \
                             Metalf, Metali, Metalu)                           \
  case HTK_##Name##_float:                                                     \
    return HBT_##Name##_float;                                                 \
  case HTK_##Name##_int:                                                       \
    return HBT_##Name##_int;                                                   \
  case HTK_##Name##_uint:                                                      \
    return HBT_##Name##_uint;
#include "BuiltinTypes.def"
  }
}

struct TextureRecord {
  StringRef Name;
  HshTextureKind Kind;
  unsigned UseStages;
};

struct VertexBinding {
  uint8_t Binding;
  uint32_t Stride;
  HshAttributeKind InputRate;
};

struct VertexAttribute {
  uint32_t Offset;
  uint8_t Binding;
  HshFormat Format;
};

struct SampleCall {
  CXXMemberCallExpr *Expr;
  unsigned Index;
  unsigned SamplerIndex;
};

struct ShaderPrintingPolicyBase : PrintingPolicy {
  HshTarget Target;
  virtual ~ShaderPrintingPolicyBase() = default;
  virtual void printStage(raw_ostream &OS, CXXRecordDecl *UniformRecord,
                          CXXRecordDecl *FromRecord, CXXRecordDecl *ToRecord,
                          ArrayRef<AttributeRecord> Attributes,
                          ArrayRef<TextureRecord> Textures,
                          ArrayRef<SamplerRecord> Samplers,
                          ArrayRef<ColorTargetRecord> ColorTargets,
                          CompoundStmt *Stmts, HshStage Stage, HshStage From,
                          HshStage To, uint32_t UniformBinding,
                          ArrayRef<SampleCall> SampleCalls) = 0;
  explicit ShaderPrintingPolicyBase(HshTarget Target)
      : PrintingPolicy(LangOptions()), Target(Target) {}
};

template <typename ImplClass>
struct ShaderPrintingPolicy : PrintingCallbacks, ShaderPrintingPolicyBase {
  HshBuiltins &Builtins;
  explicit ShaderPrintingPolicy(HshBuiltins &Builtins, HshTarget Target)
      : ShaderPrintingPolicyBase(Target), Builtins(Builtins) {
    Callbacks = this;
    Indentation = 1;
    IncludeTagDefinition = false;
    SuppressTagKeyword = true;
    SuppressScope = true;
    AnonymousTagLocations = false;
    SuppressImplicitBase = true;

    DisableTypeQualifiers = true;
    DisableListInitialization = true;
  }

  StringRef overrideBuiltinTypeName(const BuiltinType *T) const override {
    if (T->isSignedIntegerOrEnumerationType()) {
      return ImplClass::SignedInt32Spelling;
    } else if (T->isUnsignedIntegerOrEnumerationType()) {
      return ImplClass::UnsignedInt32Spelling;
    } else if (T->isSpecificBuiltinType(BuiltinType::Float)) {
      return ImplClass::Float32Spelling;
    } else if (T->isSpecificBuiltinType(BuiltinType::Double)) {
      return ImplClass::Float64Spelling;
    }
    return {};
  }

  StringRef overrideTagDeclIdentifier(TagDecl *D) const override {
    auto HBT = Builtins.identifyBuiltinType(D->getTypeForDecl());
    if (HBT == HBT_None)
      return {};
    return HshBuiltins::getSpelling<ImplClass::SourceTarget>(HBT);
  }

  StringRef overrideBuiltinFunctionIdentifier(CallExpr *C) const override {
    if (auto *MemberCall = dyn_cast<CXXMemberCallExpr>(C)) {
      auto HBM = Builtins.identifyBuiltinMethod(MemberCall->getMethodDecl());
      if (HBM == HBM_None)
        return {};
      return static_cast<const ImplClass &>(*this).identifierOfCXXMethod(
          HBM, MemberCall);
    }
    if (auto *DeclRef =
            dyn_cast<DeclRefExpr>(C->getCallee()->IgnoreParenImpCasts())) {
      if (auto *FD = dyn_cast<FunctionDecl>(DeclRef->getDecl())) {
        auto HBF = Builtins.identifyBuiltinFunction(FD);
        if (HBF == HBF_None)
          return {};
        return HshBuiltins::getSpelling<ImplClass::SourceTarget>(HBF);
      }
    }
    return {};
  }

  bool overrideCallArguments(
      CallExpr *C, const std::function<void(StringRef)> &StringArg,
      const std::function<void(Expr *)> &ExprArg) const override {
    if (auto *MemberCall = dyn_cast<CXXMemberCallExpr>(C)) {
      auto HBM = Builtins.identifyBuiltinMethod(MemberCall->getMethodDecl());
      if (HBM == HBM_None)
        return {};
      return static_cast<const ImplClass &>(*this).overrideCXXMethodArguments(
          HBM, MemberCall, StringArg, ExprArg);
    }
    return false;
  }

  StringRef overrideDeclRefIdentifier(DeclRefExpr *DR) const override {
    if (auto *PVD = dyn_cast<ParmVarDecl>(DR->getDecl())) {
      if (PVD->hasAttr<HshPositionAttr>())
        return static_cast<const ImplClass &>(*this).identifierOfVertexPosition(
            PVD);
      else if (PVD->hasAttr<HshColorTargetAttr>())
        return static_cast<const ImplClass &>(*this).identifierOfColorTarget(
            PVD);
    }
    return {};
  }

  StringRef prependMemberExprBase(MemberExpr *ME,
                                  bool &ReplaceBase) const override {
    if (auto *DRE = dyn_cast<DeclRefExpr>(ME->getBase())) {
      if (DRE->getDecl()->hasAttr<HshVertexBufferAttr>() ||
          DRE->getDecl()->hasAttr<HshInstanceBufferAttr>())
        return ImplClass::VertexBufferBase;
      if (ImplClass::NoUniformVarDecl &&
          DRE->getDecl()->getName() == "_from_host")
        ReplaceBase = true;
    }
    return {};
  }

  bool shouldPrintMemberExprUnderscore(MemberExpr *ME) const override {
    if (auto *DRE = dyn_cast<DeclRefExpr>(ME->getBase())) {
      return DRE->getDecl()->hasAttr<HshVertexBufferAttr>() ||
             DRE->getDecl()->hasAttr<HshInstanceBufferAttr>();
    }
    return false;
  }
};

struct GLSLPrintingPolicy : ShaderPrintingPolicy<GLSLPrintingPolicy> {
  static constexpr HshTarget SourceTarget = HT_GLSL;
  static constexpr bool NoUniformVarDecl = true;
  static constexpr llvm::StringLiteral SignedInt32Spelling{"int"};
  static constexpr llvm::StringLiteral UnsignedInt32Spelling{"uint"};
  static constexpr llvm::StringLiteral Float32Spelling{"float"};
  static constexpr llvm::StringLiteral Float64Spelling{"double"};
  static constexpr llvm::StringLiteral VertexBufferBase{""};

  static constexpr StringRef identifierOfVertexPosition(ParmVarDecl *PVD) {
    return llvm::StringLiteral("gl_Position");
  }

  static constexpr StringRef identifierOfColorTarget(ParmVarDecl *PVD) {
    return {};
  }

  static constexpr StringRef identifierOfCXXMethod(HshBuiltinCXXMethod HBM,
                                                   CXXMemberCallExpr *C) {
    switch (HBM) {
    case HBM_sample_texture2d:
      return llvm::StringLiteral("texture");
    default:
      return {};
    }
  }

  static constexpr bool
  overrideCXXMethodArguments(HshBuiltinCXXMethod HBM, CXXMemberCallExpr *C,
                             const std::function<void(StringRef)> &StringArg,
                             const std::function<void(Expr *)> &ExprArg) {
    switch (HBM) {
    case HBM_sample_texture2d: {
      ExprArg(C->getImplicitObjectArgument()->IgnoreParenImpCasts());
      ExprArg(C->getArg(0));
      return true;
    }
    default:
      return false;
    }
  }

  void printStage(raw_ostream &OS, CXXRecordDecl *UniformRecord,
                  CXXRecordDecl *FromRecord, CXXRecordDecl *ToRecord,
                  ArrayRef<AttributeRecord> Attributes,
                  ArrayRef<TextureRecord> Textures,
                  ArrayRef<SamplerRecord> Samplers,
                  ArrayRef<ColorTargetRecord> ColorTargets, CompoundStmt *Stmts,
                  HshStage Stage, HshStage From, HshStage To,
                  uint32_t UniformBinding,
                  ArrayRef<SampleCall> SampleCalls) override {
    OS << "#version 450 core\n";
    if (UniformRecord) {
      OS << "layout(binding = " << UniformBinding << ") uniform host_to_"
         << HshStageToString(Stage) << " {\n";
      for (auto *FD : UniformRecord->fields()) {
        OS << "  ";
        FD->print(OS, *this, 1);
        OS << ";\n";
      }
      OS << "};\n";
    }

    if (FromRecord) {
      OS << "in " << HshStageToString(From) << "_to_" << HshStageToString(Stage)
         << " {\n";
      for (auto *FD : FromRecord->fields()) {
        OS << "  ";
        FD->print(OS, *this, 1);
        OS << ";\n";
      }
      OS << "} _from_" << HshStageToString(From) << ";\n";
    }

    if (ToRecord) {
      OS << "out " << HshStageToString(Stage) << "_to_" << HshStageToString(To)
         << " {\n";
      for (auto *FD : ToRecord->fields()) {
        OS << "  ";
        FD->print(OS, *this, 1);
        OS << ";\n";
      }
      OS << "} _to_" << HshStageToString(To) << ";\n";
    }

    if (Stage == HshVertexStage) {
      uint32_t Location = 0;
      for (const auto &Attribute : Attributes) {
        for (const auto *FD : Attribute.Record->fields()) {
          QualType Tp = FD->getType().getUnqualifiedType();
          HshBuiltinType HBT = Builtins.identifyBuiltinType(Tp);
          if (HshBuiltins::isMatrixType(HBT)) {
            switch (HBT) {
            case HBT_float3x3:
              OS << "layout(location = " << Location << ") in ";
              Tp.print(OS, *this);
              OS << " " << Attribute.Name << "_" << FD->getName() << ";\n";
              Location += 3;
              break;
            case HBT_float4x4:
              OS << "layout(location = " << Location << ") in ";
              Tp.print(OS, *this);
              OS << " " << Attribute.Name << "_" << FD->getName() << ";\n";
              Location += 4;
              break;
            default:
              llvm_unreachable("Unhandled matrix type");
            }
          } else {
            OS << "layout(location = " << Location++ << ") in ";
            Tp.print(OS, *this);
            OS << " " << Attribute.Name << "_" << FD->getName() << ";\n";
          }
        }
      }
    }

    uint32_t TexBinding = 0;
    for (const auto &Tex : Textures) {
      if ((1u << Stage) & Tex.UseStages)
        OS << "layout(binding = " << TexBinding << ") uniform "
           << HshBuiltins::getSpelling<SourceTarget>(
                  BuiltinTypeOfTexture(Tex.Kind))
           << " " << Tex.Name << ";\n";
      ++TexBinding;
    }

    if (Stage == HshFragmentStage)
      for (const auto &CT : ColorTargets)
        OS << "layout(location = " << CT.Index << ") out vec4 " << CT.Name
           << ";\n";

    OS << "void main() ";
    Stmts->printPretty(OS, nullptr, *this);
  }

  using ShaderPrintingPolicy<GLSLPrintingPolicy>::ShaderPrintingPolicy;
};

struct HLSLPrintingPolicy : ShaderPrintingPolicy<HLSLPrintingPolicy> {
  static constexpr HshTarget SourceTarget = HT_HLSL;
  static constexpr bool NoUniformVarDecl = true;
  static constexpr llvm::StringLiteral SignedInt32Spelling{"int"};
  static constexpr llvm::StringLiteral UnsignedInt32Spelling{"uint"};
  static constexpr llvm::StringLiteral Float32Spelling{"float"};
  static constexpr llvm::StringLiteral Float64Spelling{"double"};
  static constexpr llvm::StringLiteral VertexBufferBase{"_vert_data."};

  std::string VertexPositionIdentifier;
  StringRef identifierOfVertexPosition(ParmVarDecl *PVD) const {
    return VertexPositionIdentifier;
  }

  mutable std::string ColorTargetIdentifier;
  StringRef identifierOfColorTarget(ParmVarDecl *PVD) const {
    ColorTargetIdentifier.clear();
    raw_string_ostream OS(ColorTargetIdentifier);
    OS << "_targets_out." << PVD->getName();
    return OS.str();
  }

  mutable std::string CXXMethodIdentifier;
  StringRef identifierOfCXXMethod(HshBuiltinCXXMethod HBM,
                                  CXXMemberCallExpr *C) const {
    switch (HBM) {
    case HBM_sample_texture2d: {
      CXXMethodIdentifier.clear();
      raw_string_ostream OS(CXXMethodIdentifier);
      C->getImplicitObjectArgument()->printPretty(OS, nullptr, *this);
      OS << ".Sample";
      return OS.str();
    }
    default:
      return {};
    }
  }

  ArrayRef<SampleCall> ThisSampleCalls;
  bool
  overrideCXXMethodArguments(HshBuiltinCXXMethod HBM, CXXMemberCallExpr *C,
                             const std::function<void(StringRef)> &StringArg,
                             const std::function<void(Expr *)> &ExprArg) const {
    switch (HBM) {
    case HBM_sample_texture2d: {
      auto Search =
          std::find_if(ThisSampleCalls.begin(), ThisSampleCalls.end(),
                       [&](const auto &Other) { return C == Other.Expr; });
      assert(Search != ThisSampleCalls.end() && "sample call must exist");
      std::string SamplerArg{"_sampler"};
      raw_string_ostream OS(SamplerArg);
      OS << Search->SamplerIndex;
      StringArg(OS.str());
      ExprArg(C->getArg(0));
      return true;
    }
    default:
      return false;
    }
  }

  bool overrideCXXOperatorCall(
      CXXOperatorCallExpr *C, raw_ostream &OS,
      const std::function<void(Expr *)> &ExprArg) const override {
    if (C->getNumArgs() == 2 && C->getOperator() == OO_Star) {
      if (HshBuiltins::isMatrixType(
              Builtins.identifyBuiltinType(C->getArg(0)->getType())) ||
          HshBuiltins::isMatrixType(
              Builtins.identifyBuiltinType(C->getArg(1)->getType()))) {
        OS << "mul(";
        ExprArg(C->getArg(0));
        OS << ", ";
        ExprArg(C->getArg(1));
        OS << ")";
        return true;
      }
    }
    return false;
  }

  bool overrideCXXTemporaryObjectExpr(
      CXXTemporaryObjectExpr *C, raw_ostream &OS,
      const std::function<void(Expr *)> &ExprArg) const override {
    if (C->getNumArgs() == 1) {
      auto DTp = Builtins.identifyBuiltinType(C->getType());
      auto STp = Builtins.identifyBuiltinType(C->getArg(0)->getType());
      switch (DTp) {
      case HBT_float3x3:
        switch (STp) {
        case HBT_float4x4:
          OS << "float4x4_to_float3x3(";
          ExprArg(C->getArg(0));
          OS << ")";
          return true;
        default:
          break;
        }
        break;
      default:
        break;
      }
    }
    return false;
  }

  CompoundStmt *ThisStmts = nullptr;
  std::string BeforeStatements;
  void
  printCompoundStatementBefore(const std::function<raw_ostream &()> &Indent,
                               CompoundStmt *CS) const override {
    if (CS == ThisStmts)
      Indent() << BeforeStatements;
  }

  std::string AfterStatements;
  void printCompoundStatementAfter(const std::function<raw_ostream &()> &Indent,
                                   CompoundStmt *CS) const override {
    if (CS == ThisStmts)
      Indent() << AfterStatements;
  }

  static constexpr llvm::StringLiteral HLSLRuntimeSupport{
      R"(static float3x3 float4x4_to_float3x3(float4x4 mtx) {
  return float3x3(mtx[0].xyz, mtx[1].xyz, mtx[2].xyz);
}
)"};

  void printStage(raw_ostream &OS, CXXRecordDecl *UniformRecord,
                  CXXRecordDecl *FromRecord, CXXRecordDecl *ToRecord,
                  ArrayRef<AttributeRecord> Attributes,
                  ArrayRef<TextureRecord> Textures,
                  ArrayRef<SamplerRecord> Samplers,
                  ArrayRef<ColorTargetRecord> ColorTargets, CompoundStmt *Stmts,
                  HshStage Stage, HshStage From, HshStage To,
                  uint32_t UniformBinding,
                  ArrayRef<SampleCall> SampleCalls) override {
    OS << HLSLRuntimeSupport;
    ThisStmts = Stmts;
    ThisSampleCalls = SampleCalls;

    if (UniformRecord) {
      OS << "cbuffer host_to_" << HshStageToString(Stage) << " : register(b"
         << UniformBinding << ") {\n";
      for (auto *FD : UniformRecord->fields()) {
        OS << "  ";
        FD->print(OS, *this, 1);
        OS << ";\n";
      }
      OS << "};\n";
    }

    if (FromRecord) {
      OS << "struct " << HshStageToString(From) << "_to_"
         << HshStageToString(Stage) << " {\n";
      uint32_t VarIdx = 0;
      for (auto *FD : FromRecord->fields()) {
        OS << "  ";
        FD->print(OS, *this, 1);
        OS << " : VAR" << VarIdx++ << ";\n";
      }
      OS << "};\n";
    }

    if (ToRecord) {
      OS << "struct " << HshStageToString(Stage) << "_to_"
         << HshStageToString(To) << " {\n"
         << "  float4 _position : SV_Position;\n";
      uint32_t VarIdx = 0;
      for (auto *FD : ToRecord->fields()) {
        OS << "  ";
        FD->print(OS, *this, 1);
        OS << " : VAR" << VarIdx++ << ";\n";
      }
      OS << "};\n";
    }

    if (Stage == HshVertexStage) {
      OS << "struct host_vert_data {\n";
      uint32_t Location = 0;
      for (const auto &Attribute : Attributes) {
        for (const auto *FD : Attribute.Record->fields()) {
          QualType Tp = FD->getType().getUnqualifiedType();
          HshBuiltinType HBT = Builtins.identifyBuiltinType(Tp);
          if (HshBuiltins::isMatrixType(HBT)) {
            switch (HBT) {
            case HBT_float3x3:
              if (Target == HT_VULKAN_SPIRV)
                OS << "  [[vk::location(" << Location << ")]] ";
              else
                OS << "  ";
              Tp.print(OS, *this);
              OS << " " << Attribute.Name << "_" << FD->getName() << " : ATTR"
                 << Location << ";\n";
              Location += 3;
              break;
            case HBT_float4x4:
              if (Target == HT_VULKAN_SPIRV)
                OS << "  [[vk::location(" << Location << ")]] ";
              else
                OS << "  ";
              Tp.print(OS, *this);
              OS << " " << Attribute.Name << "_" << FD->getName() << " : ATTR"
                 << Location << ";\n";
              Location += 4;
              break;
            default:
              llvm_unreachable("Unhandled matrix type");
            }
          } else {
            if (Target == HT_VULKAN_SPIRV)
              OS << "  [[vk::location(" << Location << ")]] ";
            else
              OS << "  ";
            Tp.print(OS, *this);
            OS << " " << Attribute.Name << "_" << FD->getName() << " : ATTR"
               << Location << ";\n";
            Location += 1;
          }
        }
      }
      OS << "};\n";
    }

    uint32_t TexBinding = 0;
    for (const auto &Tex : Textures) {
      if ((1u << Stage) & Tex.UseStages)
        OS << HshBuiltins::getSpelling<SourceTarget>(
                  BuiltinTypeOfTexture(Tex.Kind))
           << " " << Tex.Name << " : register(t" << TexBinding << ");\n";
      ++TexBinding;
    }

    uint32_t SamplerBinding = 0;
    for (const auto &Samp : Samplers) {
      if ((1u << Stage) & Samp.UseStages)
        OS << "SamplerState _sampler" << SamplerBinding << " : register(s"
           << SamplerBinding << ");\n";
      ++SamplerBinding;
    }

    if (Stage == HshFragmentStage) {
      OS << "struct color_targets_out {\n";
      for (const auto &CT : ColorTargets)
        OS << "  float4 " << CT.Name << " : SV_Target" << CT.Index << ";\n";
      OS << "};\n";
    }

    if (Stage == HshFragmentStage) {
      OS << "color_targets_out main(";
      BeforeStatements = "color_targets_out _targets_out;\n";
      AfterStatements = "return _targets_out;\n";
    } else if (ToRecord) {
      VertexPositionIdentifier.clear();
      raw_string_ostream PIO(VertexPositionIdentifier);
      PIO << "_to_" << HshStageToString(To) << "._position";
      OS << HshStageToString(Stage) << "_to_" << HshStageToString(To)
         << " main(";
      BeforeStatements.clear();
      raw_string_ostream BO(BeforeStatements);
      BO << HshStageToString(Stage) << "_to_" << HshStageToString(To) << " _to_"
         << HshStageToString(To) << ";\n";
      AfterStatements.clear();
      raw_string_ostream AO(AfterStatements);
      AO << "return _to_" << HshStageToString(To) << ";\n";
    }
    if (Stage == HshVertexStage)
      OS << "in host_vert_data _vert_data";
    else if (FromRecord)
      OS << "in " << HshStageToString(From) << "_to_" << HshStageToString(Stage)
         << " _from_" << HshStageToString(From);
    OS << ") ";
    Stmts->printPretty(OS, nullptr, *this);
  }

  using ShaderPrintingPolicy<HLSLPrintingPolicy>::ShaderPrintingPolicy;
};

static std::unique_ptr<ShaderPrintingPolicyBase>
MakePrintingPolicy(HshBuiltins &Builtins, HshTarget Target) {
  switch (Target) {
  case HT_GLSL:
    return std::make_unique<GLSLPrintingPolicy>(Builtins, Target);
  case HT_HLSL:
  case HT_DXBC:
  case HT_DXIL:
  case HT_VULKAN_SPIRV:
  case HT_METAL:
  case HT_METAL_BIN_MAC:
  case HT_METAL_BIN_IOS:
  case HT_METAL_BIN_TVOS:
    return std::make_unique<HLSLPrintingPolicy>(Builtins, Target);
  }
}

struct StageSources {
  HshTarget Target;
  std::array<std::string, HshMaxStage> Sources;
  explicit StageSources(HshTarget Target) : Target(Target) {}
};

class StagesBuilder
    : public StmtVisitor<StagesBuilder, Expr *, HshStage, HshStage> {
  ASTContext &Context;
  unsigned UseStages;

  static IdentifierInfo &getToIdent(ASTContext &Context, HshStage Stage) {
    std::string VarName;
    raw_string_ostream VNS(VarName);
    VNS << "_to_" << HshStageToString(Stage);
    return Context.Idents.get(VNS.str());
  }

  static IdentifierInfo &getFromIdent(ASTContext &Context, HshStage Stage) {
    std::string VarName;
    raw_string_ostream VNS(VarName);
    VNS << "_from_" << HshStageToString(Stage);
    return Context.Idents.get(VNS.str());
  }

  static IdentifierInfo &getFromToIdent(ASTContext &Context, HshStage From,
                                        HshStage To) {
    std::string RecordName;
    raw_string_ostream RNS(RecordName);
    RNS << HshStageToString(From) << "_to_" << HshStageToString(To);
    return Context.Idents.get(RNS.str());
  }

  class InterfaceRecord {
    CXXRecordDecl *Record = nullptr;
    SmallVector<std::pair<Expr *, FieldDecl *>, 8> Fields;
    VarDecl *Producer = nullptr;
    VarDecl *Consumer = nullptr;
    HshStage SStage = HshNoStage, DStage = HshNoStage;

    MemberExpr *createFieldReference(ASTContext &Context, Expr *E, VarDecl *VD,
                                     bool IgnoreExisting = false) {
      FieldDecl *Field = getFieldForExpr(Context, E, IgnoreExisting);
      if (!Field)
        return nullptr;
      return MemberExpr::CreateImplicit(
          Context,
          DeclRefExpr::Create(Context, {}, {}, VD, false, SourceLocation{},
                              E->getType(), VK_XValue),
          false, Field, Field->getType(), VK_XValue, OK_Ordinary);
    }

  public:
    void initializeRecord(ASTContext &Context, DeclContext *SpecDeclContext,
                          HshStage S, HshStage D) {
      Record = CXXRecordDecl::Create(Context, TTK_Struct, SpecDeclContext, {},
                                     {}, &getFromToIdent(Context, S, D));
      Record->startDefinition();

      CanQualType CDType =
          Record->getTypeForDecl()->getCanonicalTypeUnqualified();

      VarDecl *PVD =
          VarDecl::Create(Context, SpecDeclContext, {}, {},
                          &getToIdent(Context, D), CDType, nullptr, SC_None);
      Producer = PVD;

      VarDecl *CVD =
          VarDecl::Create(Context, SpecDeclContext, {}, {},
                          &getFromIdent(Context, S), CDType, nullptr, SC_None);
      Consumer = CVD;

      SStage = S;
      DStage = D;
    }

    static bool isSameComparisonOperand(Expr *E1, Expr *E2) {
      if (E1 == E2)
        return true;
      E1->setValueKind(VK_RValue);
      E2->setValueKind(VK_RValue);
      return Expr::isSameComparisonOperand(E1, E2);
    }

    FieldDecl *getFieldForExpr(ASTContext &Context, Expr *E,
                               bool IgnoreExisting = false) {
      for (auto &P : Fields) {
        if (isSameComparisonOperand(P.first, E))
          return IgnoreExisting ? nullptr : P.second;
      }
      std::string FieldName;
      raw_string_ostream FNS(FieldName);
      FNS << '_' << HshStageToString(SStage)[0] << HshStageToString(DStage)[0]
          << Fields.size();
      FieldDecl *FD = FieldDecl::Create(
          Context, Record, {}, {}, &Context.Idents.get(FNS.str()),
          E->getType().getUnqualifiedType(), {}, {}, false, ICIS_NoInit);
      FD->setAccess(AS_public);
      Record->addDecl(FD);
      Fields.push_back(std::make_pair(E, FD));
      return FD;
    }

    MemberExpr *createProducerFieldReference(ASTContext &Context, Expr *E) {
      return createFieldReference(Context, E, Producer, true);
    }

    MemberExpr *createConsumerFieldReference(ASTContext &Context, Expr *E) {
      return createFieldReference(Context, E, Consumer);
    }

    void finalizeRecord() { Record->completeDefinition(); }

    CXXRecordDecl *getRecord() const { return Record; }
  };

  std::array<InterfaceRecord, HshMaxStage>
      HostToStageRecords; /* Indexed by consumer stage */
  std::array<InterfaceRecord, HshMaxStage>
      InterStageRecords; /* Indexed by consumer stage */
  struct StageStmtList {
    SmallVector<Stmt *, 16> Stmts;
    CompoundStmt *CStmts = nullptr;
    SmallVector<std::pair<unsigned, VarDecl *>, 16> StmtDeclRefCount;
  };
  std::array<StageStmtList, HshMaxStage> StageStmts;
  std::array<SmallVector<SampleCall, 4>, HshMaxStage> SampleCalls;
  SmallVector<ParmVarDecl *, 4> UsedCaptures;
  SmallVector<AttributeRecord, 4> AttributeRecords;
  SmallVector<TextureRecord, 8> Textures;
  SmallVector<SamplerRecord, 8> Samplers;
  SmallVector<ColorTargetRecord, 2> ColorTargets;
  unsigned FinalStageCount = 0;
  SmallVector<VertexBinding, 4> VertexBindings;
  SmallVector<VertexAttribute, 4> VertexAttributes;

  AssignmentFinderInfo AssignFindInfo;
  VarDecl *OrigVarDecl = nullptr;
  std::unordered_map<Stmt *, std::array<Stmt *, HshMaxStage>> ReplacedAssigns;

  template <typename T>
  SmallVector<Expr *, 4> DoVisitExprRange(T Range, HshStage From, HshStage To) {
    SmallVector<Expr *, 4> Res;
    for (Expr *E : Range)
      Res.push_back(Visit(E, From, To));
    return Res;
  }

public:
  StagesBuilder(ASTContext &Context, DeclContext *SpecDeclContext,
                unsigned UseStages)
      : Context(Context), UseStages(UseStages) {
    for (int D = HshVertexStage; D < HshMaxStage; ++D) {
      if (UseStages & (1u << unsigned(D))) {
        HostToStageRecords[D].initializeRecord(Context, SpecDeclContext,
                                               HshHostStage, HshStage(D));
      }
    }
    for (int D = HshControlStage, S = HshVertexStage; D < HshMaxStage; ++D) {
      if (UseStages & (1u << unsigned(D))) {
        InterStageRecords[D].initializeRecord(Context, SpecDeclContext,
                                              HshStage(S), HshStage(D));
        S = D;
      }
    }
  }

  Expr *VisitStmt(Stmt *S, HshStage From, HshStage To) {
    llvm_unreachable("Unhandled statements should have been pruned already");
    return nullptr;
  }

  /* Begin ignores */
  Expr *VisitBlockExpr(BlockExpr *Block, HshStage From, HshStage To) {
    return Visit(Block->getBody(), From, To);
  }

  Expr *VisitValueStmt(ValueStmt *ValueStmt, HshStage From, HshStage To) {
    return Visit(ValueStmt->getExprStmt(), From, To);
  }

  Expr *VisitUnaryOperator(UnaryOperator *UnOp, HshStage From, HshStage To) {
    return Visit(UnOp->getSubExpr(), From, To);
  }

  Expr *VisitGenericSelectionExpr(GenericSelectionExpr *GSE, HshStage From,
                                  HshStage To) {
    return Visit(GSE->getResultExpr(), From, To);
  }

  Expr *VisitChooseExpr(ChooseExpr *CE, HshStage From, HshStage To) {
    return Visit(CE->getChosenSubExpr(), From, To);
  }

  Expr *VisitConstantExpr(ConstantExpr *CE, HshStage From, HshStage To) {
    return Visit(CE->getSubExpr(), From, To);
  }

  Expr *VisitImplicitCastExpr(ImplicitCastExpr *ICE, HshStage From,
                              HshStage To) {
    return Visit(ICE->getSubExpr(), From, To);
  }

  Expr *VisitFullExpr(FullExpr *FE, HshStage From, HshStage To) {
    return Visit(FE->getSubExpr(), From, To);
  }

  Expr *VisitMaterializeTemporaryExpr(MaterializeTemporaryExpr *MTE,
                                      HshStage From, HshStage To) {
    return Visit(MTE->getSubExpr(), From, To);
  }

  Expr *VisitSubstNonTypeTemplateParmExpr(SubstNonTypeTemplateParmExpr *NTTP,
                                          HshStage From, HshStage To) {
    return Visit(NTTP->getReplacement(), From, To);
  }
  /* End ignores */

  /*
   * Base case for createInterStageReferenceExpr.
   * Stage division will be established on this expression.
   */
  Expr *VisitExpr(Expr *E, HshStage From, HshStage To) {
    if (From == To || From == HshNoStage || To == HshNoStage)
      return E;
    if (From != HshHostStage) {
      /* Create intermediate inter-stage assignments */
      for (int D = From + 1, S = From; D <= To; ++D) {
        if (UseStages & (1u << unsigned(D))) {
          InterfaceRecord &SRecord = InterStageRecords[S];
          InterfaceRecord &DRecord = InterStageRecords[D];
          if (MemberExpr *Producer =
                  DRecord.createProducerFieldReference(Context, E))
            addStageStmt(
                new (Context) BinaryOperator(
                    Producer,
                    S == From
                        ? E
                        : SRecord.createConsumerFieldReference(Context, E),
                    BO_Assign, E->getType(), VK_XValue, OK_Ordinary, {}, {}),
                HshStage(S));
          S = D;
        }
      }
    } else {
      if (MemberExpr *Producer =
              HostToStageRecords[To].createProducerFieldReference(Context, E))
        addStageStmt(new (Context)
                         BinaryOperator(Producer, E, BO_Assign, E->getType(),
                                        VK_XValue, OK_Ordinary, {}, {}),
                     From);
    }
    InterfaceRecord &Record =
        From == HshHostStage ? HostToStageRecords[To] : InterStageRecords[To];
    return Record.createConsumerFieldReference(Context, E);
  }

  /*
   * Construction expressions are a form of component-wise type conversions in
   * hsh. They may be lifted to the target stage.
   */
  Expr *VisitCXXConstructExpr(CXXConstructExpr *ConstructExpr, HshStage From,
                              HshStage To) {
    auto Arguments = DoVisitExprRange(ConstructExpr->arguments(), From, To);
    CXXConstructExpr *NCE = CXXTemporaryObjectExpr::Create(
        Context, ConstructExpr->getConstructor(), ConstructExpr->getType(),
        Context.getTrivialTypeSourceInfo(ConstructExpr->getType()), Arguments,
        {}, ConstructExpr->hadMultipleCandidates(),
        ConstructExpr->isListInitialization(),
        ConstructExpr->isStdInitListInitialization(),
        ConstructExpr->requiresZeroInitialization());
    return NCE;
  }

  void registerReplacedAssign(Stmt *OldAssign, Stmt *NewAssign, HshStage From) {
    auto &Assigns = ReplacedAssigns[OldAssign];
    Assigns[From] = NewAssign;
  }

  std::tuple<Stmt *, Stmt *> findLastAssignment(VarDecl *&VD,
                                                HshStage From) const {
    auto Ret = LastAssignmentFinder(Context).Find(
        VD, AssignFindInfo.Body, AssignFindInfo.LastCompoundChild);
    auto Search = ReplacedAssigns.find(std::get<0>(Ret));
    if (Search != ReplacedAssigns.end()) {
      if (auto *NewAssign = Search->second[From]) {
        VD = cast<VarDecl>(cast<DeclStmt>(NewAssign)->getSingleDecl());
        return std::make_tuple(NewAssign, std::get<1>(Ret));
      }
    }
    return Ret;
  }

  /*
   * DeclRef expressions may connect directly to a construction expression and
   * should therefore be lifted to the target stage.
   */
  Expr *VisitDeclRefExpr(DeclRefExpr *DeclRef, HshStage From, HshStage To) {
    if (auto *VD = dyn_cast<VarDecl>(DeclRef->getDecl())) {
      if (auto *PVD = dyn_cast<ParmVarDecl>(DeclRef->getDecl()))
        return VisitExpr(DeclRef, From, To);
      OrigVarDecl = VD;
      auto [Assign, NextCompoundChild] = findLastAssignment(VD, From);
      if (Assign) {
        SaveAndRestore<Stmt *> SavedCompoundChild(
            AssignFindInfo.LastCompoundChild, NextCompoundChild);
        SaveAndRestore<VarDecl *> SavedSelectedVarDecl(
            AssignFindInfo.SelectedVarDecl, VD);
        return Visit(Assign, From, To);
      }
    }
    llvm_unreachable("Should have been handled already");
    return nullptr;
  }

  Expr *VisitDeclStmt(DeclStmt *DeclStmt, HshStage From, HshStage To) {
    for (Decl *D : DeclStmt->getDeclGroup()) {
      if (auto *VD = dyn_cast<VarDecl>(D)) {
        if (VD == AssignFindInfo.SelectedVarDecl) {
          auto *NVD = VarDecl::Create(
              Context, VD->getDeclContext(), {}, {}, VD->getIdentifier(),
              VD->getType().getUnqualifiedType(), nullptr, SC_None);
          auto *NDS = new (Context) class DeclStmt(DeclGroupRef(NVD), {}, {});
          if (Expr *Init = VD->getInit())
            NVD->setInit(Visit(Init, From, To));
          registerReplacedAssign(DeclStmt, NDS, To);
          liftDeclStmt(NDS, From, To, OrigVarDecl);
          return DeclRefExpr::Create(
              Context, {}, {}, NVD, true, SourceLocation{},
              VD->getType().getNonReferenceType(), VK_RValue);
        }
      }
    }
    llvm_unreachable("Should have been handled already");
    return nullptr;
  }

  /*
   * Certain trivial expressions like type conversions may be lifted into
   * the target stage rather than creating redundant inter-stage data.
   */
  Expr *createInterStageReferenceExpr(Expr *E, HshStage From, HshStage To,
                                      const AssignmentFinderInfo &AFI) {
    AssignFindInfo = AFI;
    return Visit(E, From, To);
  }

  void addStageStmt(Stmt *S, HshStage Stage, VarDecl *OrigDecl = nullptr) {
    if (auto *DS = dyn_cast<DeclStmt>(S)) {
      auto RefCountIt = StageStmts[Stage].StmtDeclRefCount.begin();
      for (auto I = StageStmts[Stage].Stmts.begin(),
                E = StageStmts[Stage].Stmts.end();
           I != E; ++I, ++RefCountIt) {
        if (isa<DeclStmt>(*I)) {
          if (RefCountIt->second == OrigDecl) {
            ++RefCountIt->first;
            return;
          }
        }
      }
    } else {
      for (Stmt *ES : StageStmts[Stage].Stmts)
        if (ES == S)
          return;
    }
    StageStmts[Stage].Stmts.push_back(S);
    StageStmts[Stage].StmtDeclRefCount.push_back({1, OrigDecl});
  }

  void liftDeclStmt(DeclStmt *DS, HshStage From, HshStage To,
                    VarDecl *OrigDecl) {
    addStageStmt(DS, To, OrigDecl);
    auto RefCountIt = StageStmts[From].StmtDeclRefCount.begin();
    for (auto I = StageStmts[From].Stmts.begin(),
              E = StageStmts[From].Stmts.end();
         I != E; ++I, ++RefCountIt) {
      if (isa<DeclStmt>(*I)) {
        if (RefCountIt->second == OrigDecl) {
          if (--RefCountIt->first == 0) {
            StageStmts[From].Stmts.erase(I);
            StageStmts[From].StmtDeclRefCount.erase(RefCountIt);
          }
          break;
        }
      }
    }
  }

  template <typename TexAttr>
  std::pair<HshStage, APSInt> getTextureIndex(TexAttr *A) {
    Expr::EvalResult Res;
    A->getIndex()->EvaluateAsInt(Res, Context);
    return {StageOfTextureAttr<TexAttr>(), Res.Val.getInt()};
  }

  template <typename TexAttr>
  std::pair<HshStage, APSInt> getTextureIndex(ParmVarDecl *PVD) {
    if (auto *A = PVD->getAttr<TexAttr>())
      return getTextureIndex(A);
    return {HshNoStage, APSInt{}};
  }

  template <typename TexAttrA, typename TexAttrB, typename... Rest>
  std::pair<HshStage, APSInt> getTextureIndex(ParmVarDecl *PVD) {
    if (auto *A = PVD->getAttr<TexAttrA>())
      return getTextureIndex(A);
    return getTextureIndex<TexAttrB, Rest...>(PVD);
  }

  std::pair<HshStage, APSInt> getTextureIndex(ParmVarDecl *PVD) {
    return getTextureIndex<HshVertexTextureAttr, HshFragmentTextureAttr>(PVD);
  }

  void registerSampleCall(HshBuiltinCXXMethod HBM, CXXMemberCallExpr *C) {
    if (auto *DR = dyn_cast<DeclRefExpr>(
            C->getImplicitObjectArgument()->IgnoreParenImpCasts())) {
      if (auto *PVD = dyn_cast<ParmVarDecl>(DR->getDecl())) {
        auto [TexStage, TexIdx] = getTextureIndex(PVD);
        auto &StageCalls = SampleCalls[TexStage];
        for (const auto &Call : StageCalls)
          if (Call.Expr == C)
            return;
        APValue Res;
        Expr *SamplerArg = C->getArg(1);
        if (!SamplerArg->isCXX11ConstantExpr(Context, &Res)) {
          ReportNonConstexprSampler(SamplerArg, Context);
          return;
        }
        if (!SamplerConfig::ValidateSamplerStruct(Res)) {
          ReportBadSamplerStructFormat(SamplerArg, Context);
          return;
        }
        SamplerConfig Sampler(Res);
        auto Search =
            std::find_if(Samplers.begin(), Samplers.end(),
                         [&](const auto &S) { return S.Config == Sampler; });
        if (Search == Samplers.end()) {
          if (Samplers.size() == HSH_MAX_SAMPLERS) {
            ReportSamplerOverflow(SamplerArg, Context);
            return;
          }
          Search = Samplers.insert(Samplers.end(),
                                   SamplerRecord{Sampler, 1u << TexStage});
        } else {
          Search->UseStages |= 1u << TexStage;
        }
        StageCalls.push_back({C, unsigned(TexIdx.getZExtValue()),
                              unsigned(Search - Samplers.begin())});
      }
    }
  }

  void registerUsedCapture(ParmVarDecl *PVD) {
    for (auto *EC : UsedCaptures)
      if (EC == PVD)
        return;
    UsedCaptures.push_back(PVD);
  }

  auto captures() const {
    return llvm::iterator_range(UsedCaptures.begin(), UsedCaptures.end());
  }

  void registerAttributeRecord(AttributeRecord Attribute) {
    auto Search =
        std::find_if(AttributeRecords.begin(), AttributeRecords.end(),
                     [&](const auto &A) { return A.Name == Attribute.Name; });
    if (Search != AttributeRecords.end())
      return;
    AttributeRecords.push_back(Attribute);
  }

  void registerTexture(StringRef Name, HshTextureKind Kind, HshStage Stage) {
    auto Search = std::find_if(Textures.begin(), Textures.end(),
                               [&](const auto &T) { return T.Name == Name; });
    if (Search != Textures.end()) {
      Search->UseStages |= 1u << Stage;
      return;
    }
    Textures.push_back(TextureRecord{Name, Kind, 1u << Stage});
  }

  void registerColorTarget(ColorTargetRecord Record) {
    auto Search =
        std::find_if(ColorTargets.begin(), ColorTargets.end(),
                     [&](const auto &T) { return T.Name == Record.Name; });
    if (Search != ColorTargets.end())
      return;
    ColorTargets.push_back(Record);
  }

  ArrayRef<Stmt *> hostStatements() const {
    return StageStmts[HshHostStage].Stmts;
  }

  void finalizeResults(ASTContext &Context, HshBuiltins &Builtins,
                       CXXRecordDecl *SpecRecord) {
    FinalStageCount = 0;
    for (int D = HshVertexStage; D < HshMaxStage; ++D) {
      if (UseStages & (1u << unsigned(D))) {
        auto &RecDecl = HostToStageRecords[D];
        RecDecl.finalizeRecord();
        SpecRecord->addDecl(RecDecl.getRecord());
        ++FinalStageCount;
      }
    }

    for (int D = HshControlStage; D < HshMaxStage; ++D) {
      if (UseStages & (1u << unsigned(D)))
        InterStageRecords[D].finalizeRecord();
    }

    auto &HostStmts = StageStmts[HshHostStage];
    std::array<VarDecl *, HshMaxStage> HostToStageVars{};
    SmallVector<Stmt *, 16> NewHostStmts;
    NewHostStmts.reserve(HostStmts.Stmts.size() + HshMaxStage * 2);
    for (int S = HshVertexStage; S < HshMaxStage; ++S) {
      if (UseStages & (1u << unsigned(S))) {
        auto *RecordDecl = HostToStageRecords[S].getRecord();
        CanQualType CDType =
            RecordDecl->getTypeForDecl()->getCanonicalTypeUnqualified();
        VarDecl *BindingVar = VarDecl::Create(Context, SpecRecord, {}, {},
                                              &getToIdent(Context, HshStage(S)),
                                              CDType, {}, SC_None);
        HostToStageVars[S] = BindingVar;
        NewHostStmts.push_back(new (Context)
                                   DeclStmt(DeclGroupRef(BindingVar), {}, {}));
      }
    }
    NewHostStmts.insert(NewHostStmts.end(), HostStmts.Stmts.begin(),
                        HostStmts.Stmts.end());
    for (int S = HshVertexStage; S < HshMaxStage; ++S) {
      if (auto *VD = HostToStageVars[S])
        NewHostStmts.push_back(
            Builtins.getPushUniformCall(Context, VD, HshStage(S)));
    }
    HostStmts.Stmts = std::move(NewHostStmts);

    for (int S = HshHostStage; S < HshMaxStage; ++S) {
      if (UseStages & (1u << unsigned(S))) {
        auto &Stmts = StageStmts[S];
        Stmts.CStmts = CompoundStmt::Create(Context, Stmts.Stmts, {}, {});
      }
    }
  }

  HshStage previousUsedStage(HshStage S) const {
    for (int D = S - 1; D >= HshVertexStage; --D) {
      if (UseStages & (1u << unsigned(D)))
        return HshStage(D);
    }
    return HshNoStage;
  }

  HshStage nextUsedStage(HshStage S) const {
    for (int D = S + 1; D < HshMaxStage; ++D) {
      if (UseStages & (1u << unsigned(D)))
        return HshStage(D);
    }
    return HshNoStage;
  }

  StageSources printResults(ShaderPrintingPolicyBase &Policy) {
    StageSources Sources(Policy.Target);

    uint32_t UniformBinding = 0;
    for (int S = HshVertexStage; S < HshMaxStage; ++S) {
      if (UseStages & (1u << unsigned(S))) {
        raw_string_ostream OS(Sources.Sources[S]);
        HshStage NextStage = nextUsedStage(HshStage(S));
        Policy.printStage(
            OS, HostToStageRecords[S].getRecord(),
            InterStageRecords[S].getRecord(),
            NextStage != HshNoStage ? InterStageRecords[NextStage].getRecord()
                                    : nullptr,
            AttributeRecords, Textures, Samplers, ColorTargets,
            StageStmts[S].CStmts, HshStage(S), previousUsedStage(HshStage(S)),
            NextStage, UniformBinding++, SampleCalls[S]);
      }
    }

    return Sources;
  }

  unsigned getNumStages() const { return FinalStageCount; }
  unsigned getNumBindings() const { return VertexBindings.size(); }
  unsigned getNumAttributes() const { return VertexAttributes.size(); }
  ArrayRef<VertexBinding> getBindings() const { return VertexBindings; }
  ArrayRef<VertexAttribute> getAttributes() const { return VertexAttributes; }
};

using StmtResult = std::pair<Stmt *, HshStage>;
class ValueTracer : public StmtVisitor<ValueTracer, StmtResult> {
  ASTContext &Context;
  const HshBuiltins &Builtins;
  StagesBuilder &Builder;
  AssignmentFinderInfo AssignFindInfo;
  HshStage Target = HshNoStage;
  bool InMemberExpr = false;

  static constexpr StmtResult ErrorResult{nullptr, HshNoStage};

  bool GetInterpolated(HshStage Stage) const {
    return Stage != HshHostStage && Stage < Target;
  }

  struct VisitExprRangeResult {
    HshStage Stage = HshNoStage;
    SmallVector<Expr *, 4> Exprs;
    SmallVector<HshStage, 4> ExprStages;
    operator ArrayRef<Expr *>() { return Exprs; }
  };

  template <typename T>
  Optional<VisitExprRangeResult> DoVisitExprRange(T Range, Stmt *Parent) {
    VisitExprRangeResult Res;
    for (Expr *E : Range) {
      auto [ExprStmt, ExprStage] = Visit(E);
      if (!ExprStmt)
        return {};
      Res.Exprs.push_back(cast<Expr>(ExprStmt));
      Res.ExprStages.emplace_back(ExprStage);
      Res.Stage = std::max(Res.Stage, ExprStage);
    }
    return {Res};
  }

  void DoPromoteExprRange(VisitExprRangeResult &Res) {
    auto ExprStageI = Res.ExprStages.begin();
    for (Expr *&E : Res.Exprs) {
      E = Builder.createInterStageReferenceExpr(E, *ExprStageI, Res.Stage,
                                                AssignFindInfo);
      ++ExprStageI;
    }
  }

public:
  explicit ValueTracer(ASTContext &Context, const HshBuiltins &Builtins,
                       StagesBuilder &Promotions)
      : Context(Context), Builtins(Builtins), Builder(Promotions) {}

  /* Begin ignores */
  StmtResult VisitBlockExpr(BlockExpr *Block) {
    return Visit(Block->getBody());
  }

  StmtResult VisitValueStmt(ValueStmt *ValueStmt) {
    return Visit(ValueStmt->getExprStmt());
  }

  StmtResult VisitUnaryOperator(UnaryOperator *UnOp) {
    return Visit(UnOp->getSubExpr());
  }

  StmtResult VisitGenericSelectionExpr(GenericSelectionExpr *GSE) {
    return Visit(GSE->getResultExpr());
  }

  StmtResult VisitChooseExpr(ChooseExpr *CE) {
    return Visit(CE->getChosenSubExpr());
  }

  StmtResult VisitConstantExpr(ConstantExpr *CE) {
    return Visit(CE->getSubExpr());
  }

  StmtResult VisitImplicitCastExpr(ImplicitCastExpr *ICE) {
    return Visit(ICE->getSubExpr());
  }

  StmtResult VisitFullExpr(FullExpr *FE) { return Visit(FE->getSubExpr()); }

  StmtResult VisitMaterializeTemporaryExpr(MaterializeTemporaryExpr *MTE) {
    return Visit(MTE->getSubExpr());
  }

  StmtResult
  VisitSubstNonTypeTemplateParmExpr(SubstNonTypeTemplateParmExpr *NTTP) {
    return Visit(NTTP->getReplacement());
  }
  /* End ignores */

  StmtResult VisitStmt(Stmt *S) {
    ReportUnsupportedStmt(S, Context);
    return ErrorResult;
  }

  StmtResult VisitDeclStmt(DeclStmt *DeclStmt) {
    for (Decl *D : DeclStmt->getDeclGroup()) {
      if (auto *VD = dyn_cast<VarDecl>(D)) {
        if (VD == AssignFindInfo.SelectedVarDecl) {
          auto *NVD = VarDecl::Create(
              Context, VD->getDeclContext(), {}, {}, VD->getIdentifier(),
              VD->getType().getUnqualifiedType(), nullptr, SC_None);
          HshStage Stage = HshNoStage;
          if (Expr *Init = VD->getInit()) {
            auto [InitStmt, InitStage] = Visit(Init);
            if (!InitStmt)
              return ErrorResult;
            NVD->setInit(cast<Expr>(InitStmt));
            Stage = InitStage;
          }
          auto *NDS = new (Context) class DeclStmt(DeclGroupRef(NVD), {}, {});
          Builder.registerReplacedAssign(DeclStmt, NDS, Stage);
          return {NDS, Stage};
        }
      }
    }
    return ErrorResult;
  }

  static StmtResult VisitNullStmt(NullStmt *NS) { return {NS, HshNoStage}; }

  StmtResult VisitBinaryOperator(BinaryOperator *BinOp) {
    auto [LStmt, LStage] = Visit(BinOp->getLHS());
    if (!LStmt)
      return ErrorResult;
    auto [RStmt, RStage] = Visit(BinOp->getRHS());
    if (!RStmt)
      return ErrorResult;
    HshStage Stage = std::max(LStage, RStage);

    const bool LHSInterpolated = GetInterpolated(LStage);
    const bool RHSInterpolated = GetInterpolated(RStage);
    if (LHSInterpolated || RHSInterpolated) {
      switch (BinOp->getOpcode()) {
      case BO_Add:
      case BO_Sub:
      case BO_Mul:
      case BO_AddAssign:
      case BO_SubAssign:
      case BO_MulAssign:
      case BO_Assign:
        break;
      case BO_Div:
      case BO_DivAssign:
        if (RHSInterpolated)
          Stage = Target;
        break;
      default:
        Stage = Target;
        break;
      }
    }

    LStmt = Builder.createInterStageReferenceExpr(cast<Expr>(LStmt), LStage,
                                                  Stage, AssignFindInfo);
    RStmt = Builder.createInterStageReferenceExpr(cast<Expr>(RStmt), RStage,
                                                  Stage, AssignFindInfo);
    auto *NewBinOp = new (Context)
        BinaryOperator(cast<Expr>(LStmt), cast<Expr>(RStmt), BinOp->getOpcode(),
                       BinOp->getType(), VK_XValue, OK_Ordinary, {}, {});

    return {NewBinOp, Stage};
  }

  StmtResult VisitExpr(Expr *E) {
    ReportUnsupportedStmt(E, Context);
    return ErrorResult;
  }

  StmtResult VisitCallExpr(CallExpr *CallExpr) {
    if (auto *DeclRef = dyn_cast<DeclRefExpr>(
            CallExpr->getCallee()->IgnoreParenImpCasts())) {
      if (auto *FD = dyn_cast<FunctionDecl>(DeclRef->getDecl())) {
        HshBuiltinFunction Func = Builtins.identifyBuiltinFunction(FD);
        if (Func != HBF_None) {
          auto Arguments = DoVisitExprRange(CallExpr->arguments(), CallExpr);
          if (!Arguments)
            return ErrorResult;

          if (CallExpr->getNumArgs() == 2) {
            const bool LHSInterpolated =
                GetInterpolated(Arguments->ExprStages[0]);
            const bool RHSInterpolated =
                GetInterpolated(Arguments->ExprStages[1]);
            if ((LHSInterpolated || RHSInterpolated) &&
                !HshBuiltins::isInterpolationDistributed(Func))
              Arguments->Stage = Target;
          }

          DoPromoteExprRange(*Arguments);
          auto *NCE =
              CallExpr::Create(Context, CallExpr->getCallee(), *Arguments,
                               CallExpr->getType(), VK_XValue, {});
          return {NCE, Arguments->Stage};
        }
      }
    }
    ReportUnsupportedFunctionCall(CallExpr, Context);
    return ErrorResult;
  }

  StmtResult VisitCXXMemberCallExpr(CXXMemberCallExpr *CallExpr) {
    CXXMethodDecl *MD = CallExpr->getMethodDecl();
    Expr *ObjArg = CallExpr->getImplicitObjectArgument()->IgnoreParenImpCasts();
    HshBuiltinCXXMethod Method = Builtins.identifyBuiltinMethod(MD);
    if (HshBuiltins::isSwizzleMethod(Method)) {
      auto [BaseStmt, BaseStage] = Visit(ObjArg);
      auto *ME = MemberExpr::CreateImplicit(Context, cast<Expr>(BaseStmt),
                                            false, MD, MD->getReturnType(),
                                            VK_XValue, OK_Ordinary);
      return {ME, BaseStage};
    }
    switch (Method) {
    case HBM_sample_texture2d: {
      HshStage Stage = HshNoStage;
      ParmVarDecl *PVD = nullptr;
      if (auto *TexRef = dyn_cast<DeclRefExpr>(ObjArg))
        PVD = dyn_cast<ParmVarDecl>(TexRef->getDecl());
      if (PVD) {
        if (PVD->hasAttr<HshVertexTextureAttr>())
          Stage = HshVertexStage;
        else if (PVD->hasAttr<HshFragmentTextureAttr>())
          Stage = HshFragmentStage;
        else
          ReportUnattributedTexture(PVD, Context);
      } else {
        ReportBadTextureReference(CallExpr, Context);
      }
      auto [UVStmt, UVStage] = Visit(CallExpr->getArg(0));
      if (!UVStmt)
        return ErrorResult;
      UVStmt = Builder.createInterStageReferenceExpr(
          cast<Expr>(UVStmt), UVStage, Stage, AssignFindInfo);
      std::array<Expr *, 2> NewArgs{cast<Expr>(UVStmt), CallExpr->getArg(1)};
      auto *NMCE =
          CXXMemberCallExpr::Create(Context, CallExpr->getCallee(), NewArgs,
                                    CallExpr->getType(), VK_XValue, {});
      Builder.registerSampleCall(Method, NMCE);
      return {NMCE, Stage};
    }
    default:
      ReportUnsupportedFunctionCall(CallExpr, Context);
      break;
    }
    return ErrorResult;
  }

  StmtResult VisitCastExpr(CastExpr *CastExpr) {
    if (Builtins.identifyBuiltinType(CastExpr->getType()) == HBT_None) {
      ReportUnsupportedTypeCast(CastExpr, Context);
      return ErrorResult;
    }
    return Visit(CastExpr->getSubExpr());
  }

  StmtResult VisitCXXConstructExpr(CXXConstructExpr *ConstructExpr) {
    if (Builtins.identifyBuiltinType(ConstructExpr->getType()) == HBT_None) {
      ReportUnsupportedTypeConstruct(ConstructExpr, Context);
      return ErrorResult;
    }

    auto Arguments =
        DoVisitExprRange(ConstructExpr->arguments(), ConstructExpr);
    if (!Arguments)
      return ErrorResult;
    DoPromoteExprRange(*Arguments);
    CXXConstructExpr *NCE = CXXTemporaryObjectExpr::Create(
        Context, ConstructExpr->getConstructor(), ConstructExpr->getType(),
        Context.getTrivialTypeSourceInfo(ConstructExpr->getType()), *Arguments,
        {}, ConstructExpr->hadMultipleCandidates(),
        ConstructExpr->isListInitialization(),
        ConstructExpr->isStdInitListInitialization(),
        ConstructExpr->requiresZeroInitialization());
    return {NCE, Arguments->Stage};
  }

  StmtResult VisitCXXOperatorCallExpr(CXXOperatorCallExpr *CallExpr) {
    auto Arguments = DoVisitExprRange(CallExpr->arguments(), CallExpr);
    if (!Arguments)
      return ErrorResult;

    if (CallExpr->getNumArgs() == 2) {
      const bool LHSInterpolated = GetInterpolated(Arguments->ExprStages[0]);
      const bool RHSInterpolated = GetInterpolated(Arguments->ExprStages[1]);
      if (LHSInterpolated || RHSInterpolated) {
        switch (CallExpr->getOperator()) {
        case OO_Plus:
        case OO_Minus:
        case OO_Star:
        case OO_PlusEqual:
        case OO_MinusEqual:
        case OO_StarEqual:
        case OO_Equal:
          break;
        case OO_Slash:
        case OO_SlashEqual:
          if (RHSInterpolated)
            Arguments->Stage = Target;
          break;
        default:
          Arguments->Stage = Target;
          break;
        }
      }
    }

    DoPromoteExprRange(*Arguments);
    auto *NCE = CXXOperatorCallExpr::Create(
        Context, CallExpr->getOperator(), CallExpr->getCallee(), *Arguments,
        CallExpr->getType(), VK_XValue, {}, {});
    return {NCE, Arguments->Stage};
  }

  StmtResult VisitDeclRefExpr(DeclRefExpr *DeclRef) {
    if (auto *VD = dyn_cast<VarDecl>(DeclRef->getDecl())) {
      if (!InMemberExpr &&
          Builtins.identifyBuiltinType(VD->getType()) == HBT_None) {
        ReportUnsupportedTypeReference(DeclRef, Context);
        return ErrorResult;
      }
      if (auto *PVD = dyn_cast<ParmVarDecl>(DeclRef->getDecl())) {
        HshStage Stage = DetermineParmVarStage(PVD);
        if (Stage == HshHostStage) {
          if (!CheckHshFieldTypeCompatibility(Builtins, Context, PVD))
            return ErrorResult;
          Builder.registerUsedCapture(PVD);
        }
        return {DeclRef, Stage};
      }
      auto [Assign, NextCompoundChild] = LastAssignmentFinder(Context).Find(
          VD, AssignFindInfo.Body, AssignFindInfo.LastCompoundChild);
      if (Assign) {
        SaveAndRestore<Stmt *> SavedCompoundChild(
            AssignFindInfo.LastCompoundChild, NextCompoundChild);
        SaveAndRestore<VarDecl *> SavedSelectedVarDecl(
            AssignFindInfo.SelectedVarDecl, VD);
        auto [AssignStmt, AssignStage] = Visit(Assign);
        if (!AssignStmt)
          return ErrorResult;
        Builder.addStageStmt(AssignStmt, AssignStage, VD);
        return {DeclRef, AssignStage};
      }
    }
    return ErrorResult;
  }

  StmtResult VisitInitListExpr(InitListExpr *InitList) {
    auto Exprs = DoVisitExprRange(InitList->inits(), InitList);
    if (!Exprs)
      return ErrorResult;
    DoPromoteExprRange(*Exprs);
    return {new (Context) InitListExpr(Context, {}, *Exprs, {}), Exprs->Stage};
  }

  StmtResult VisitMemberExpr(MemberExpr *MemberExpr) {
    if (!InMemberExpr &&
        Builtins.identifyBuiltinType(MemberExpr->getType()) == HBT_None) {
      ReportUnsupportedTypeReference(MemberExpr, Context);
      return ErrorResult;
    }
    SaveAndRestore<bool> SavedInMemberExpr(InMemberExpr, true);
    auto [BaseStmt, BaseStage] = Visit(MemberExpr->getBase());
    auto *NME = MemberExpr::CreateImplicit(
        Context, cast<Expr>(BaseStmt), false, MemberExpr->getMemberDecl(),
        MemberExpr->getType(), VK_XValue, OK_Ordinary);
    return {NME, BaseStage};
  }

  static StmtResult VisitFloatingLiteral(FloatingLiteral *FL) {
    return {FL, HshNoStage};
  }

  static StmtResult VisitIntegerLiteral(IntegerLiteral *IL) {
    return {IL, HshNoStage};
  }

  void Trace(Stmt *Assign, Stmt *B, Stmt *LCC, HshStage T) {
    AssignFindInfo.Body = B;
    AssignFindInfo.LastCompoundChild = LCC;
    Target = T;
    auto [AssignStmt, AssignStage] = Visit(Assign);
    if (!AssignStmt)
      return;
    AssignStmt = Builder.createInterStageReferenceExpr(
        cast<Expr>(AssignStmt), AssignStage, T, AssignFindInfo);
    Builder.addStageStmt(AssignStmt, T);
  }
};

struct StageBinaries {
  HshTarget Target;
  std::array<std::pair<std::vector<uint8_t>, uint64_t>, HshMaxStage> Binaries;
  void updateHashes() {
    for (auto &Binary : Binaries)
      if (!Binary.first.empty())
        Binary.second = xxHash64(Binary.first);
  }
  explicit StageBinaries(HshTarget Target) : Target(Target) {}
};

class StagesCompilerBase {
protected:
  virtual StageBinaries doCompile() const = 0;

public:
  virtual ~StagesCompilerBase() = default;
  StageBinaries compile() const {
    auto Binaries = doCompile();
    Binaries.updateHashes();
    return Binaries;
  }
};

class StagesCompilerText : public StagesCompilerBase {
  const StageSources &Sources;

protected:
  StageBinaries doCompile() const override {
    StageBinaries Binaries(Sources.Target);
    auto OutIt = Binaries.Binaries.begin();
    for (const auto &Stage : Sources.Sources) {
      auto &Out = OutIt++->first;
      if (Stage.empty())
        continue;
      Out.resize(Stage.size() + 1);
      std::memcpy(&Out[0], Stage.data(), Stage.size());
    }
    return Binaries;
  }

public:
  explicit StagesCompilerText(const StageSources &Sources) : Sources(Sources) {}
};

class StagesCompilerDXIL : public StagesCompilerBase {
  const StageSources &Sources;
  DiagnosticsEngine &Diags;

  static constexpr std::array<LPCWSTR, 6> ShaderProfiles{
      nullptr, L"vs_6_0", L"hs_6_0", L"ds_6_0", L"gs_6_0", L"ps_6_0"};

protected:
  StageBinaries doCompile() const override {
    CComPtr<IDxcCompiler3> Compiler =
        DxcLibrary::SharedInstance->MakeCompiler();
    StageBinaries Binaries(Sources.Target);
    auto OutIt = Binaries.Binaries.begin();
    auto ProfileIt = ShaderProfiles.begin();
    int StageIt = 0;
    for (const auto &Stage : Sources.Sources) {
      auto &Out = OutIt++->first;
      const LPCWSTR Profile = *ProfileIt++;
      const auto HStage = HshStage(StageIt++);
      if (Stage.empty())
        continue;
      DxcText SourceBuf{Stage.data(), Stage.size(), 0};
      LPCWSTR DxArgs[] = {L"-T", Profile};
      LPCWSTR VkArgs[] = {L"-T", Profile, L"-spirv"};
      LPCWSTR *Args = Sources.Target == HT_VULKAN_SPIRV ? VkArgs : DxArgs;
      UINT32 ArgCount = Sources.Target == HT_VULKAN_SPIRV
                            ? std::extent_v<decltype(VkArgs)>
                            : std::extent_v<decltype(DxArgs)>;
      CComPtr<IDxcResult> Result;
      HRESULT HResult = Compiler->Compile(&SourceBuf, Args, ArgCount, nullptr,
                                          HSH_IID_PPV_ARGS(&Result));
      if (!Result) {
        Diags.Report(Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                           "no result from dxcompiler"));
        continue;
      }
      bool HasObj = Result->HasOutput(DXC_OUT_OBJECT);
      if (HasObj) {
        CComPtr<IDxcBlob> ObjBlob;
        Result->GetOutput(DXC_OUT_OBJECT, HSH_IID_PPV_ARGS(&ObjBlob), nullptr);
        if (auto Size = ObjBlob->GetBufferSize()) {
          Out.resize(Size);
          std::memcpy(&Out[0], ObjBlob->GetBufferPointer(), Size);
        } else {
          HasObj = false;
        }
      }
      if (Result->HasOutput(DXC_OUT_ERRORS)) {
        CComPtr<IDxcBlobUtf8> ErrBlob;
        Result->GetOutput(DXC_OUT_ERRORS, HSH_IID_PPV_ARGS(&ErrBlob), nullptr);
        if (ErrBlob->GetBufferSize()) {
          if (!HasObj)
            llvm::errs() << Stage << '\n';
          Diags.Report(Diags.getCustomDiagID(HasObj ? DiagnosticsEngine::Warning
                                                    : DiagnosticsEngine::Error,
                                             "%0 problem from dxcompiler: %1"))
              << HshStageToString(HStage)
              << (char *)ErrBlob->GetBufferPointer();
        }
      }
      if (HResult != ERROR_SUCCESS) {
        Diags.Report(Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                           "%0 problem from dxcompiler: %1"))
            << HshStageToString(HStage) << HResult;
      }
    }
    return Binaries;
  }

public:
  explicit StagesCompilerDXIL(const StageSources &Sources, StringRef ProgramDir,
                              DiagnosticsEngine &Diags)
      : Sources(Sources), Diags(Diags) {
    DxcLibrary::EnsureSharedInstance(ProgramDir, Diags);
  }
};

static std::unique_ptr<StagesCompilerBase>
MakeCompiler(const StageSources &Sources, StringRef ProgramDir,
             DiagnosticsEngine &Diags) {
  switch (Sources.Target) {
  case HT_GLSL:
  case HT_HLSL:
    return std::make_unique<StagesCompilerText>(Sources);
  case HT_DXBC:
  case HT_DXIL:
  case HT_VULKAN_SPIRV:
    return std::make_unique<StagesCompilerDXIL>(Sources, ProgramDir, Diags);
  case HT_METAL:
  case HT_METAL_BIN_MAC:
  case HT_METAL_BIN_IOS:
  case HT_METAL_BIN_TVOS:
    return std::make_unique<StagesCompilerText>(Sources);
  }
}

class LocationNamespaceSearch
    : public RecursiveASTVisitor<LocationNamespaceSearch> {
  ASTContext &Context;
  SourceLocation L;
  NamespaceDecl *InNS = nullptr;

public:
  explicit LocationNamespaceSearch(ASTContext &Context) : Context(Context) {}

  bool VisitNamespaceDecl(NamespaceDecl *NS) {
    auto Range = NS->getSourceRange();
    if (Range.getBegin() < L && L < Range.getEnd()) {
      InNS = NS;
      return false;
    }
    return true;
  }

  NamespaceDecl *findNamespace(SourceLocation Location) {
    L = Location;
    InNS = nullptr;
    TraverseAST(Context);
    return InNS;
  }
};

class GenerateConsumer : public ASTConsumer, MatchFinder::MatchCallback {
  HshBuiltins Builtins;
  CompilerInstance &CI;
  ASTContext &Context;
  Preprocessor &PP;
  StringRef ProgramDir;
  ArrayRef<HshTarget> Targets;
  std::unique_ptr<raw_pwrite_stream> OS;
  std::unordered_set<uint64_t> SeenHashes;
  std::string AnonNSString;
  raw_string_ostream AnonOS{AnonNSString};
  Optional<std::pair<SourceLocation, std::string>> HeadInclude;
  std::map<SourceLocation, std::pair<SourceRange, std::string>>
      SeenHshExpansions;

public:
  explicit GenerateConsumer(CompilerInstance &CI, StringRef ProgramDir,
                            ArrayRef<HshTarget> Targets)
      : CI(CI), Context(CI.getASTContext()), PP(CI.getPreprocessor()),
        ProgramDir(ProgramDir), Targets(Targets) {}

  void run(const MatchFinder::MatchResult &Result) override {
    auto *LambdaAttr = Result.Nodes.getNodeAs<AttributedStmt>("attrid");
    auto *Lambda = Result.Nodes.getNodeAs<LambdaExpr>("id");
    if (Lambda && LambdaAttr) {
      auto ExpName = getExpansionNameBeforeLambda(LambdaAttr);
      assert(!ExpName.empty() && "Expansion name should exist");

      auto *CallOperator = Lambda->getCallOperator();
      Stmt *Body = CallOperator->getBody();

      unsigned UseStages = 1;

      std::array<ParmVarDecl *, HSH_MAX_VERTEX_BUFFERS> VertexBufferParms{};
      auto CheckVertexBufferParm = [&](ParmVarDecl *PVD, auto *Attr) {
        bool Ret = true;
        auto NonRefType = PVD->getType().getNonReferenceType();
        if (!NonRefType->isStructureOrClassType()) {
          ReportBadVertexBufferType(PVD, Context);
          Ret = false;
        } else if (!CheckHshRecordCompatibility(
                       Builtins, Context, NonRefType->getAsCXXRecordDecl())) {
          Ret = false;
        }
        llvm::APSInt Res;
        Attr->getIndex()->isIntegerConstantExpr(Res, Context);
        if (Res < 0 || Res >= HSH_MAX_VERTEX_BUFFERS) {
          ReportVertexBufferOutOfRange(PVD, Context);
          Ret = false;
        }
        if (auto *OtherPVD = VertexBufferParms[Res.getExtValue()]) {
          ReportVertexBufferDuplicate(PVD, OtherPVD, Context);
          Ret = false;
        }
        if (!Ret)
          return false;
        VertexBufferParms[Res.getExtValue()] = PVD;
        return true;
      };

      std::array<std::array<ParmVarDecl *, HSH_MAX_TEXTURES>, HshMaxStage>
          TextureParms{};
      auto CheckTextureParm = [&](ParmVarDecl *PVD, auto *Attr,
                                  HshStage Stage) {
        auto &StageTextureParms = TextureParms[Stage];
        bool Ret = true;
        if (!HshBuiltins::isTextureType(
                Builtins.identifyBuiltinType(PVD->getType()))) {
          ReportBadTextureType(PVD, Context);
          Ret = false;
        }
        llvm::APSInt Res;
        Attr->getIndex()->isIntegerConstantExpr(Res, Context);
        if (Res < 0 || Res >= HSH_MAX_TEXTURES) {
          ReportTextureOutOfRange(PVD, Context);
          Ret = false;
        }
        if (auto *OtherPVD = StageTextureParms[Res.getExtValue()]) {
          ReportTextureDuplicate(PVD, OtherPVD, Context);
          Ret = false;
        }
        if (!Ret)
          return false;
        StageTextureParms[Res.getExtValue()] = PVD;
        return true;
      };

      std::array<ParmVarDecl *, HSH_MAX_COLOR_TARGETS> ColorTargetParms{};
      auto CheckColorTargetParm = [&](ParmVarDecl *PVD,
                                      HshColorTargetAttr *Attr) {
        bool Ret = true;
        if (Builtins.identifyBuiltinType(PVD->getType()) != HBT_float4) {
          ReportBadColorTargetType(PVD, Context);
          Ret = false;
        }
        llvm::APSInt Res;
        Attr->getIndex()->isIntegerConstantExpr(Res, Context);
        if (Res < 0 || Res >= HSH_MAX_COLOR_TARGETS) {
          ReportColorTargetOutOfRange(PVD, Context);
          Ret = false;
        }
        if (!Ret)
          return false;
        ColorTargetParms[Res.getExtValue()] = PVD;
        return true;
      };

      for (ParmVarDecl *Param : CallOperator->parameters()) {
        HshInterfaceDirection Direction = DetermineParmVarDirection(Param);
        if (Direction != HshInput) {
          if (Param->hasAttr<HshPositionAttr>()) {
            if (Builtins.identifyBuiltinType(Param->getType()) != HBT_float4) {
              ReportBadVertexPositionType(Param, Context);
              return;
            }
          } else if (auto *CA = Param->getAttr<HshColorTargetAttr>()) {
            if (!CheckColorTargetParm(Param, CA))
              return;
          }
          UseStages |= (1u << unsigned(DetermineParmVarStage(Param)));
        } else {
          if (auto *VB = Param->getAttr<HshVertexBufferAttr>()) {
            if (!CheckVertexBufferParm(Param, VB))
              return;
          } else if (auto *IB = Param->getAttr<HshInstanceBufferAttr>()) {
            if (!CheckVertexBufferParm(Param, IB))
              return;
          } else if (auto *VTA = Param->getAttr<HshVertexTextureAttr>()) {
            if (!CheckTextureParm(Param, VTA, HshVertexStage))
              return;
          } else if (auto *FTA = Param->getAttr<HshFragmentTextureAttr>()) {
            if (!CheckTextureParm(Param, FTA, HshFragmentStage))
              return;
          }
        }
      }

      CXXRecordDecl *SpecRecord =
          Builtins.getHshBaseSpecialization(Context, ExpName);
      StagesBuilder Builder(Context, SpecRecord, UseStages);

      for (uint8_t i = 0; i < HSH_MAX_VERTEX_BUFFERS; ++i) {
        if (ParmVarDecl *VertexBufferParm = VertexBufferParms[i]) {
          Builder.registerAttributeRecord(
              {VertexBufferParm->getName(),
               VertexBufferParm->getType()
                   .getNonReferenceType()
                   ->getAsCXXRecordDecl(),
               VertexBufferParm->hasAttr<HshVertexBufferAttr>() ? PerVertex
                                                                : PerInstance,
               i});
        }
      }

      for (int s = HshVertexStage; s < HshMaxStage; ++s) {
        for (uint8_t i = 0; i < HSH_MAX_TEXTURES; ++i) {
          if (ParmVarDecl *TextureParm = TextureParms[s][i]) {
            Builder.registerTexture(
                TextureParm->getName(),
                KindOfTextureType(
                    Builtins.identifyBuiltinType(TextureParm->getType())),
                HshStage(s));
          }
        }
      }

      for (uint8_t i = 0; i < HSH_MAX_COLOR_TARGETS; ++i) {
        if (ParmVarDecl *CTParm = ColorTargetParms[i]) {
          Builder.registerColorTarget(ColorTargetRecord{CTParm->getName(), i});
        }
      }

      for (int i = HshVertexStage; i < HshMaxStage; ++i) {
        for (ParmVarDecl *Param : CallOperator->parameters()) {
          if (DetermineParmVarDirection(Param) == HshInput ||
              DetermineParmVarStage(Param) != HshStage(i))
            continue;
          auto [Assign, LastCompoundChild] =
              LastAssignmentFinder(Context).Find(Param, Body);
          if (Context.getDiagnostics().hasErrorOccurred())
            return;
          if (Assign)
            ValueTracer(Context, Builtins, Builder)
                .Trace(Assign, Body, LastCompoundChild, HshStage(i));
        }
      }

      // Add global list node static
      SpecRecord->addDecl(Builtins.getGlobalListNode(Context, SpecRecord));

      // Finalize expressions and add host to stage records
      Builder.finalizeResults(Context, Builtins, SpecRecord);

      // Set public access
      SpecRecord->addDecl(
          AccessSpecDecl::Create(Context, AS_public, SpecRecord, {}, {}));

      // Make constructor
      SmallVector<QualType, 4> ConstructorArgs;
      SmallVector<ParmVarDecl *, 4> ConstructorParms;
      for (const auto *Cap : Builder.captures()) {
        ConstructorArgs.push_back(
            Cap->getType().isPODType(Context)
                ? Cap->getType()
                : Context.getLValueReferenceType(Cap->getType().withConst()));
        ConstructorParms.push_back(ParmVarDecl::Create(
            Context, SpecRecord, {}, {}, Cap->getIdentifier(),
            ConstructorArgs.back(), {}, SC_None, nullptr));
      }
      CanQualType CDType =
          SpecRecord->getTypeForDecl()->getCanonicalTypeUnqualified();
      CXXConstructorDecl *CD = CXXConstructorDecl::Create(
          Context, SpecRecord, {},
          {Context.DeclarationNames.getCXXConstructorName(CDType), {}},
          Context.getFunctionType(CDType, ConstructorArgs, {}), {},
          {nullptr, ExplicitSpecKind::ResolvedTrue}, false, false,
          CSK_unspecified);
      CD->setParams(ConstructorParms);
      CD->setAccess(AS_public);
      CD->setBody(
          CompoundStmt::Create(Context, Builder.hostStatements(), {}, {}));
      SpecRecord->addDecl(CD);

      // Add shader data var template
      SpecRecord->addDecl(Builtins.getDataVarTemplate(
          Context, SpecRecord, Builder.getNumStages(), Builder.getNumBindings(),
          Builder.getNumAttributes()));

      SpecRecord->completeDefinition();

      // Emit shader record
      SpecRecord->print(AnonOS, Context.getPrintingPolicy());
      AnonOS << ";\nhsh::_HshGlobalListNode " << ExpName << "::global{&"
             << ExpName << "::global_build};\n";

      // Emit shader data
      for (auto Target : Targets) {
        auto Policy = MakePrintingPolicy(Builtins, Target);
        auto Sources = Builder.printResults(*Policy);
        auto Compiler =
            MakeCompiler(Sources, ProgramDir, Context.getDiagnostics());
        if (Context.getDiagnostics().hasErrorOccurred())
          return;
        auto Binaries = Compiler->compile();
        auto SourceIt = Sources.Sources.begin();
        int StageIt = HshHostStage;

        AnonOS << "template <> hsh::_HshShaderData<";
        Builtins.printTargetEnumString(AnonOS, Context.getPrintingPolicy(),
                                       Target);
        AnonOS << ", " << Builder.getNumStages() << ", "
               << Builder.getNumBindings() << ", " << Builder.getNumAttributes()
               << "> " << ExpName << "::data<";
        Builtins.printTargetEnumString(AnonOS, Context.getPrintingPolicy(),
                                       Target);
        AnonOS << ">{\n";

        for (auto &[Data, Hash] : Binaries.Binaries) {
          auto &Source = *SourceIt++;
          auto Stage = HshStage(StageIt++);
          if (Data.empty())
            continue;
          auto HashStr = llvm::utohexstr(Hash);
          AnonOS << "  _hsho_" << HashStr << ",\n";
          if (SeenHashes.find(Hash) != SeenHashes.end())
            continue;
          SeenHashes.insert(Hash);
          {
            raw_comment_ostream CommentOut(*OS);
            CommentOut << HshStageToString(Stage) << " source targeting "
                       << HshTargetToString(Binaries.Target) << "\n\n";
            CommentOut << Source;
          }
          *OS << "inline ";
          {
            raw_carray_ostream DataOut(*OS, "_hshs_"s + llvm::utohexstr(Hash));
            DataOut.write((const char *)Data.data(), Data.size());
          }
          *OS << "\ninline hsh::_HshShaderObject<";
          Builtins.printTargetEnumString(*OS, Context.getPrintingPolicy(),
                                         Target);
          *OS << "> _hsho_" << HashStr << "{";
          Builtins.printStageEnumString(*OS, Context.getPrintingPolicy(),
                                        Stage);
          *OS << ", {_hshs_" << HashStr << ", 0x" << HashStr << "}};\n\n";
        }

        for (const auto &Binding : Builder.getBindings()) {
          AnonOS << "  hsh::_HshVertexBinding{" << Binding.Binding << ", "
                 << Binding.Stride << ", ";
          Builtins.printInputRateEnumString(AnonOS, Context.getPrintingPolicy(),
                                            Binding.InputRate);
          AnonOS << "},\n";
        }

        for (const auto &Attribute : Builder.getAttributes()) {
          AnonOS << "  hsh::_HshVertexAttribute{" << Attribute.Binding << ", ";
          Builtins.printFormatEnumString(AnonOS, Context.getPrintingPolicy(),
                                         Attribute.Format);
          AnonOS << ", " << Attribute.Offset << "},\n";
        }

        AnonOS << "};\n";
      }

      // Emit define macro for capturing args
      AnonOS << "#define " << ExpName << " ::" << ExpName << "(";
      bool NeedsComma = false;
      for (const auto *Cap : Builder.captures()) {
        if (!NeedsComma)
          NeedsComma = true;
        else
          AnonOS << ", ";
        AnonOS << Cap->getIdentifier()->getName();
      }
      AnonOS << "); (void)\n\n";

      ASTDumper P(llvm::errs(), nullptr, &Context.getSourceManager());
      P.Visit(Body);
    }
  }

  void HandleTranslationUnit(ASTContext &Context) override {
    DiagnosticsEngine &Diags = Context.getDiagnostics();
    if (Diags.hasErrorOccurred())
      return;

    const unsigned IncludeDiagID =
        Diags.getCustomDiagID(DiagnosticsEngine::Error,
                              "hshhead include in must appear in global scope");
    if (!HeadInclude) {
      Diags.Report(IncludeDiagID);
      return;
    }
    if (NamespaceDecl *NS = LocationNamespaceSearch(Context).findNamespace(
            HeadInclude->first)) {
      Diags.Report(HeadInclude->first, IncludeDiagID);
      Diags.Report(NS->getLocation(),
                   Diags.getCustomDiagID(DiagnosticsEngine::Note,
                                         "included in namespace"));
      return;
    }

    Builtins.findBuiltinDecls(Context);
    if (Context.getDiagnostics().hasErrorOccurred())
      return;

    OS = CI.createDefaultOutputFile(false);

    SourceManager &SM = Context.getSourceManager();
    StringRef MainName = SM.getFileEntryForID(SM.getMainFileID())->getName();
    *OS << "/* Auto-generated hshhead for " << MainName << " */\n\n";

    AnonOS << "namespace {\n\n";

    /*
     * Find lambdas that are attributed with hsh::generator_lambda and exist
     * within the main file.
     */
    MatchFinder Finder;
    Finder.addMatcher(
        attributedStmt(stmt().bind("attrid"),
                       allOf(hasStmtAttr(attr::HshGeneratorLambda),
                             hasDescendant(lambdaExpr(
                                 stmt().bind("id"), isExpansionInMainFile())))),
        this);
    Finder.matchAST(Context);

    AnonOS << "}\n";

    *OS << AnonOS.str();

    DxcLibrary::SharedInstance.reset();
  }

  StringRef
  getExpansionNameBeforeLambda(const AttributedStmt *LambdaAttr) const {
    for (auto *Attr : LambdaAttr->getAttrs()) {
      if (Attr->getKind() == attr::HshGeneratorLambda) {
        PresumedLoc PLoc =
            Context.getSourceManager().getPresumedLoc(Attr->getLoc());
        for (const auto &Exp : SeenHshExpansions) {
          PresumedLoc IPLoc =
              Context.getSourceManager().getPresumedLoc(Exp.first);
          if (IPLoc.getLine() == PLoc.getLine())
            return Exp.second.second;
        }
      }
    }
    return {};
  }

  void registerHshHeadInclude(SourceLocation HashLoc,
                              CharSourceRange FilenameRange,
                              StringRef RelativePath) {
    if (Context.getSourceManager().isWrittenInMainFile(HashLoc)) {
      DiagnosticsEngine &Diags = Context.getDiagnostics();
      if (HeadInclude) {
        Diags.Report(HashLoc, Diags.getCustomDiagID(
                                  DiagnosticsEngine::Error,
                                  "multiple hshhead includes in one file"));
        Diags.Report(HeadInclude->first,
                     Diags.getCustomDiagID(DiagnosticsEngine::Note,
                                           "previous include was here"));
        return;
      } else {
        std::string ExpectedName =
            sys::path::filename(CI.getFrontendOpts().OutputFile);
        if (ExpectedName != RelativePath) {
          std::string Replacement = "\""s + ExpectedName + '\"';
          Diags.Report(
              FilenameRange.getBegin(),
              Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                    "hshhead include must match the output "
                                    "filename"))
              << FixItHint::CreateReplacement(FilenameRange, Replacement);
          return;
        }
        HeadInclude.emplace(HashLoc, RelativePath);
      }
    }
  }

  void registerHshExpansion(SourceRange Range, StringRef Name) {
    if (Context.getSourceManager().isWrittenInMainFile(Range.getBegin())) {
      for (auto &Exps : SeenHshExpansions) {
        if (Exps.second.second == Name) {
          DiagnosticsEngine &Diags = Context.getDiagnostics();
          Diags.Report(
              Range.getBegin(),
              Diags.getCustomDiagID(DiagnosticsEngine::Error,
                                    "hsh_* macro must be suffixed with "
                                    "identifier unique to the file"))
              << CharSourceRange(Range, false);
          Diags.Report(Exps.first, Diags.getCustomDiagID(
                                       DiagnosticsEngine::Note,
                                       "previous identifier usage is here"))
              << CharSourceRange(Exps.second.first, false);
          return;
        }
      }
      SeenHshExpansions[Range.getBegin()] = std::make_pair(Range, Name);
    }
  }

  class PPCallbacks : public clang::PPCallbacks {
    GenerateConsumer &Consumer;
    FileManager &FM;
    SourceManager &SM;

  public:
    explicit PPCallbacks(GenerateConsumer &Consumer, FileManager &FM,
                         SourceManager &SM)
        : Consumer(Consumer), FM(FM), SM(SM) {}
    bool FileNotFound(StringRef FileName,
                      SmallVectorImpl<char> &RecoveryPath) override {
      if (FileName.endswith_lower(llvm::StringLiteral(".hshhead"))) {
        SmallString<1024> VirtualFilePath(llvm::StringLiteral("./"));
        VirtualFilePath += FileName;
        FM.getVirtualFile(VirtualFilePath, 0, std::time(nullptr));
        RecoveryPath.push_back('.');
        return true;
      }
      return false;
    }
    void InclusionDirective(SourceLocation HashLoc, const Token &IncludeTok,
                            StringRef FileName, bool IsAngled,
                            CharSourceRange FilenameRange,
                            const FileEntry *File, StringRef SearchPath,
                            StringRef RelativePath,
                            const clang::Module *Imported,
                            SrcMgr::CharacteristicKind FileType) override {
      if (FileName.endswith_lower(llvm::StringLiteral(".hshhead"))) {
        assert(File && "File must exist at this point");
        SM.overrideFileContents(File, llvm::MemoryBuffer::getMemBuffer(""));
        Consumer.registerHshHeadInclude(HashLoc, FilenameRange, RelativePath);
      }
    }
    void MacroExpands(const Token &MacroNameTok, const MacroDefinition &MD,
                      SourceRange Range, const MacroArgs *Args) override {
      if (MacroNameTok.is(tok::identifier)) {
        StringRef Name = MacroNameTok.getIdentifierInfo()->getName();
        if (Name.startswith("hsh_"))
          Consumer.registerHshExpansion(Range, Name);
      }
    }
  };
};

std::unique_ptr<ASTConsumer>
GenerateAction::CreateASTConsumer(CompilerInstance &CI, StringRef InFile) {
  auto Policy = CI.getASTContext().getPrintingPolicy();
  Policy.Indentation = 1;
  Policy.SuppressImplicitBase = true;
  CI.getASTContext().setPrintingPolicy(Policy);
  auto Consumer = std::make_unique<GenerateConsumer>(CI, ProgramDir, Targets);
  CI.getPreprocessor().addPPCallbacks(
      std::make_unique<GenerateConsumer::PPCallbacks>(
          *Consumer, CI.getFileManager(), CI.getSourceManager()));
  return Consumer;
}

} // namespace clang::hshgen
